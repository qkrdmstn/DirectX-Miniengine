// 저장소 루트의 VS Developer PowerShell에서 아래 명령을 실행한다.
// cl /std:c++17 /EHsc /utf-8 "/I$env:TEMP/SP7b/Debug/Output/ModelViewer" "/Fo:$env:TEMP/SP7b/" "/Fe:$env:TEMP/SP7b/MaskTests.exe" MiniEngine/ModelViewer/Scene/tests/TriangleMaskShaderTests.cpp d3d12.lib dxgi.lib
// 구성별 생성 헤더를 사용하며 모든 출력은 %TEMP%/SP7b에 둔다.
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <CompiledShaders/TriangleMaskCS.h>
using Microsoft::WRL::ComPtr;

// 실패한 D3D 호출을 테스트 실패로 전파한다.
void Check(HRESULT result)
{
    // Release에서도 실패를 숨기지 않는다.
    if (FAILED(result)) throw std::runtime_error("D3D call failed");
}

// 테스트용 버퍼를 만들고 업로드 메모리를 초기화한다.
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 bytes, D3D12_HEAP_TYPE heap,
    D3D12_RESOURCE_STATES state, const void* data = nullptr)
{
    // 기본 버퍼만 UAV를 허용하고 업로드와 readback은 CPU 접근 용도로 둔다.
    D3D12_HEAP_PROPERTIES properties{};
    properties.Type = heap;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    desc.Flags = heap == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> resource;
    Check(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource)));

    // 원본과 draw 입력은 GPU 실행 전에 한 번만 채운다.
    if (data)
    {
        void* mapped; Check(resource->Map(0, nullptr, &mapped));
        std::memcpy(mapped, data, static_cast<size_t>(bytes)); resource->Unmap(0, nullptr);
    }
    return resource;
}

// 하드웨어에서 혼합 포맷과 R16 공유 DWORD를 반복 실행하고 독립 CPU 기대값과 비교한다.
int main()
{
    // 검증 계층과 실제 어댑터를 준비하고 GPU 식별자를 남긴다.
    try
    {
        ComPtr<ID3D12Debug> debug; Check(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
        ComPtr<IDXGIFactory6> factory; Check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter1> adapter;
        Check(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
        DXGI_ADAPTER_DESC1 adapterDesc{}; Check(adapter->GetDesc1(&adapterDesc));
        std::printf("GPU=%ls\n", adapterDesc.Description);
        ComPtr<ID3D12Device> device; Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)));
        ComPtr<ID3D12InfoQueue> info; Check(device.As(&info));

        // root 상수와 세 SRV 및 출력 UAV를 셰이더의 register 순서로 연결한다.
        D3D12_ROOT_PARAMETER roots[5]{};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        roots[0].Constants.Num32BitValues = 4;
        for (UINT i = 1; i < 5; ++i)
        {
            roots[i].ParameterType = i == 4 ? D3D12_ROOT_PARAMETER_TYPE_UAV : D3D12_ROOT_PARAMETER_TYPE_SRV;
            roots[i].Descriptor.ShaderRegister = i == 4 ? 0 : i - 1;
        }
        D3D12_ROOT_SIGNATURE_DESC rootDesc{ 5, roots, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> serialized, errors;
        Check(D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors));
        ComPtr<ID3D12RootSignature> signature;
        Check(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&signature)));
        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};
        psoDesc.pRootSignature = signature.Get(); psoDesc.CS = { g_pTriangleMaskCS, sizeof(g_pTriangleMaskCS) };
        ComPtr<ID3D12PipelineState> pso; Check(device->CreateComputePipelineState(&psoDesc, IID_PPV_ARGS(&pso)));

        // R16 draw 둘은 한 DWORD의 양쪽을 공유하고 R32는 별도 offset을 사용한다.
        std::array<std::array<UINT, 16>, 3> draws{};
        draws[0][2] = 16; draws[0][4] = 1; draws[0][5] = 17; draws[0][6] = 2;
        draws[1][2] = 16; draws[1][4] = 7; draws[1][5] = 29; draws[1][6] = 3; draws[1][8] = 2;
        draws[2][2] = 96; draws[2][3] = 1; draws[2][4] = 2; draws[2][5] = 41; draws[2][6] = 3; draws[2][8] = 5;
        std::vector<UINT> original(128);
        for (UINT i = 0; i < original.size(); ++i) original[i] = 0xabc00000u + i * 7919;
        const auto bytes = original.size() * sizeof(UINT);
        auto source = Buffer(device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, original.data());
        auto inputs = Buffer(device.Get(), sizeof(draws), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, draws.data());
        auto state = Buffer(device.Get(), 8 * sizeof(UINT), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        std::vector<UINT> sentinels(original.size(), 0xdeadbeefu);
        auto initial = Buffer(device.Get(), bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, sentinels.data());
        auto output = Buffer(device.Get(), bytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST);
        auto readback = Buffer(device.Get(), bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);

        // 명령 목록과 fence를 재사용하며 각 readback 완료 뒤 다음 State를 업로드한다.
        D3D12_COMMAND_QUEUE_DESC queueDesc{};
        ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
        ComPtr<ID3D12CommandAllocator> allocator;
        Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
        ComPtr<ID3D12GraphicsCommandList> list;
        Check(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)));
        Check(list->Close());
        ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        UINT mismatches = 0;
        for (UINT sample = 0; sample < 13; ++sample)
        {
            // 소유권만 설정한 경우와 개별 제거 및 전체 제거 후 해제를 포함한다.
            std::array<UINT, 8> mask{};
            for (UINT t = 0; t < 8; ++t)
                mask[t] = (sample == 1 ? 65535u : 1u) | ((sample >= 2 && sample < 10 && t == sample - 2)
                    || sample == 10 || (sample == 11 && t % 2 == 0) ? 0x80000000u : 0);
            void* mapped; Check(state->Map(0, nullptr, &mapped));
            std::memcpy(mapped, mask.data(), sizeof(mask)); state->Unmap(0, nullptr);
            auto expected = original;
            expected.front() = expected.back() = sentinels.front();
            for (const auto& draw : draws)
            {
                const UINT width = draw[3] ? 4 : 2;
                for (UINT t = 0; t < draw[6]; ++t)
                {
                    if (!(mask[draw[8] + t] & 0x80000000u)) continue;
                    const UINT start = draw[2] + (draw[4] + t * 3) * width;
                    for (UINT corner = 0; corner < 3; ++corner)
                        std::memcpy(reinterpret_cast<char*>(expected.data()) + start + corner * width,
                            reinterpret_cast<const char*>(original.data()) + start, width);
                }
            }

            // 출력에는 이전 결과가 남아 있어도 매번 원본으로 재구성해야 한다.
            Check(allocator->Reset()); Check(list->Reset(allocator.Get(), pso.Get()));
            if (sample == 0)
            {
                // 자원 안의 양끝에 원본과 다른 값을 두어 범위 밖 쓰기를 검출한다.
                list->CopyResource(output.Get(), initial.Get());
                D3D12_RESOURCE_BARRIER initialize{};
                initialize.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                initialize.Transition = { output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                    D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
                list->ResourceBarrier(1, &initialize);
            }

            // 시작 DWORD를 건너뛰고 마지막 DWORD 전에 끝내며 여분 스레드도 제출한다.
            list->SetComputeRootSignature(signature.Get());
            const UINT parameters[] = { 1, static_cast<UINT>(original.size()) - 2, 3, 0 };
            list->SetComputeRoot32BitConstants(0, 4, parameters, 0);
            list->SetComputeRootShaderResourceView(1, source->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(2, inputs->GetGPUVirtualAddress());
            list->SetComputeRootShaderResourceView(3, state->GetGPUVirtualAddress());
            list->SetComputeRootUnorderedAccessView(4, output->GetGPUVirtualAddress());
            list->Dispatch(3, 1, 1);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = { output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE };
            list->ResourceBarrier(1, &barrier); list->CopyResource(readback.Get(), output.Get());
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier);
            Check(list->Close()); ID3D12CommandList* submitted[] = { list.Get() }; queue->ExecuteCommandLists(1, submitted);
            Check(queue->Signal(fence.Get(), sample + 1)); Check(fence->SetEventOnCompletion(sample + 1, nullptr));
            Check(readback->Map(0, nullptr, &mapped));
            for (size_t i = 0; i < original.size(); ++i) mismatches += static_cast<UINT*>(mapped)[i] != expected[i];
            readback->Unmap(0, nullptr);
        }

        // 오류와 손상 메시지가 하나라도 있으면 GPU 결과와 무관하게 실패한다.
        UINT validationErrors = 0;
        for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i)
        {
            SIZE_T size = 0; Check(info->GetMessage(i, nullptr, &size));
            std::vector<char> storage(size); auto message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            Check(info->GetMessage(i, message, &size));
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) ++validationErrors;
        }
        std::printf("TriangleMask GPU samples=13 comparedWords=1664 mismatches=%u D3D12_ERROR=%u\n", mismatches, validationErrors);
        return mismatches || validationErrors ? 1 : 0;
    }
    catch (const std::exception& error)
    {
        // 호출 실패도 자동 실행의 종료 코드로 보고한다.
        std::fprintf(stderr, "%s\n", error.what()); return 1;
    }
}
