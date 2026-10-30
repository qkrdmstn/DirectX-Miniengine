#pragma once

#include "../../../Core/CommandContext.h"
#include "../../../Core/CommandListManager.h"
#include "../../../Core/GraphicsCore.h"
#include "../../../Core/GpuBuffer.h"
#include "../../../Core/ReadbackBuffer.h"
#include "../../../Core/Utility.h"
#include <array>
#include <cstring>
#include <stdexcept>

namespace Viewer
{
    namespace RetiredResourceChecks
    {
        // 검사 실패 원인을 queue 단계와 함께 예외로 전달한다.
        inline void Require(bool condition, const char* message)
        {
            // 조건이 거짓이면 원인을 예외로 전달해 검사를 중단한다.
            if (!condition)
                throw std::runtime_error(message);
        }

        // gate 해제 뒤 검사 중 제출한 queue 작업까지 완료시켜 지역 buffer를 안전하게 정리한다.
        struct GateWaitGuard
        {
            CommandListManager& Manager;
            Microsoft::WRL::ComPtr<ID3D12Fence> Gate;
            uint64_t ProducerFence = 0;
            uint64_t CopyFence = 0;
            bool GateSignaled = false;

            // 검사 대상 command manager를 저장한다.
            explicit GateWaitGuard(CommandListManager& manager) : Manager(manager) {}

            // 스코프를 벗어나기 전에 CPU signal로 대기를 풀고 추적한 fence를 기다린다.
            ~GateWaitGuard()
            {
                if (Gate && !GateSignaled)
                    Gate->Signal(1);
                if (ProducerFence)
                    Manager.WaitForFence(ProducerFence);
                if (CopyFence)
                    Manager.WaitForFence(CopyFence);
            }
        };

        // 세 queue 각각에서 미완료 fence 중 buffer wrapper를 파괴해 지연 해제를 검증한다.
        inline void Run()
        {
            CommandListManager& manager = Graphics::g_CommandManager;

            // 진단 검사 시작 전 GPU를 비우고 retired 목록의 기준 상태를 확인한다.
            manager.IdleGPU();
            manager.CollectRetiredResources();
            Require(manager.GetRetiredResourceCount() == 0, "Retired resource baseline count is not zero");
            Require(manager.GetRetiredResourceBytes() == 0, "Retired resource baseline bytes are not zero");

            // graphics, compute, copy queue를 순서대로 막고 같은 GPU copy 수명을 검사한다.
            const D3D12_COMMAND_LIST_TYPE queueTypes[] = {
                D3D12_COMMAND_LIST_TYPE_DIRECT,
                D3D12_COMMAND_LIST_TYPE_COMPUTE,
                D3D12_COMMAND_LIST_TYPE_COPY
            };
            for (D3D12_COMMAND_LIST_TYPE type : queueTypes)
            {
                // queue와 fence gate를 준비하고 다음 queue 작업이 gate를 기다리게 한다.
                CommandQueue& queue = manager.GetQueue(type);
                Require(queue.IsReady() && queue.GetCommandQueue() != nullptr, "Command queue is not ready");
                Require(Graphics::g_Device != nullptr, "D3D12 device is not ready");
                ByteAddressBuffer buffer;
                ReadbackBuffer readback;
                GateWaitGuard gate(manager);
                HRESULT hr = Graphics::g_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                    IID_PPV_ARGS(gate.Gate.GetAddressOf()));
                Require(SUCCEEDED(hr), "CreateFence failed for retired resource check");
                hr = queue.GetCommandQueue()->Wait(gate.Gate.Get(), 1);
                Require(SUCCEEDED(hr), "Queue wait failed for retired resource check");
                gate.ProducerFence = queue.IncrementFence();

                // 초기화 없는 16 byte source와 readback을 만들어 direct queue copy를 기록한다.
                buffer.Create(L"Retired resource check source", 4, sizeof(uint32_t));
                readback.Create(L"Retired resource check readback", 4, sizeof(uint32_t));
                GraphicsContext& context = GraphicsContext::Begin(L"Retired resource fence check");
                context.FillBuffer(buffer, 0, 0x12345678, 16);
                context.CopyBuffer(readback, buffer);
                gate.CopyFence = context.Finish(false);

                // resource wrapper를 파괴한 뒤에도 모든 snapshot fence 전까지 manager 보관을 확인한다.
                manager.RetireResource(buffer.GetResource());
                buffer.Destroy();
                manager.CollectRetiredResources();
                Require(manager.GetRetiredResourceCount() == 1, "Retired resource was collected before all queues completed");
                Require(manager.GetRetiredResourceBytes() == 16, "Retired resource byte count is not 16");

                // CPU gate signal로 producer를 풀고 producer와 실제 copy fence를 완료한다.
                hr = gate.Gate->Signal(1);
                gate.GateSignaled = SUCCEEDED(hr);
                Require(SUCCEEDED(hr), "CPU gate signal failed for retired resource check");
                manager.WaitForFence(gate.ProducerFence);
                manager.WaitForFence(gate.CopyFence);
                manager.CollectRetiredResources();
                Require(manager.GetRetiredResourceCount() == 0, "Retired resource remains after queue completion");
                Require(manager.GetRetiredResourceBytes() == 0, "Retired resource bytes remain after queue completion");

                // GPU copy 결과의 네 uint가 모두 기록됐는지 확인한다.
                std::array<uint32_t, 4> observed{};
                void* mapped = readback.Map();
                Require(mapped != nullptr, "Readback map returned null in retired resource check");
                std::memcpy(observed.data(), mapped, sizeof(observed));
                readback.Unmap();
                for (uint32_t value : observed)
                    Require(value == 0x12345678, "GPU copy result mismatch in retired resource check");
            }

            // 세 queue 모두 pending resource 없이 결과 검증을 마쳤음을 기록한다.
            Utility::Printf("Retired resource checks passed: queues=3 pending=0 bytes=0\n");
        }
    }
}
