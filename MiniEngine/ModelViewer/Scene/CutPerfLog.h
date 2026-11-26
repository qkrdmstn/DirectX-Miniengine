#pragma once
// 단일 절단 성능 로그다. -cut-perf-log <파일 경로>가 있을 때만 켜지며 기본은 꺼짐이다.
// 켜지면 요청부터 게시까지의 CPU·GPU 구간과 프레임별 CPU·GPU 시간을 그 파일에 한 줄씩 남긴다.
// 보조 옵션: -cut-perf-quiet 1(PatchLog 파일 기록 끔), -cut-perf-gpu 0(GPU timestamp·프로파일 블록 끔),
// -cut-perf-only-target 1(대상 외 오브젝트 그리기 생략), -cut-perf-focus 1(카메라를 대상에 맞춤),
// -cut-perf-second-bone <본 이름>(첫 게시 뒤 같은 프로세스에서 두 번째 절단).
#include "CommandContext.h"
#include "CommandListManager.h"
#include "GraphicsCore.h"
#include "Display.h"
#include "ReadbackBuffer.h"
#include "EngineProfiling.h"
#include "Util/CommandLineArg.h"
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include <dxgi1_4.h>
#include <wrl/client.h>

namespace Viewer
{
    namespace CutPerf
    {
        // 로그는 메모리에 모아 두었다가 크기가 차거나 종료할 때 파일에 쓴다. 측정 구간 안의 파일 I/O를 피하기 위해서다.
        inline FILE*& LogFile() { static FILE* file = nullptr; return file; }
        inline std::string& LogBuffer() { static std::string buffer; return buffer; }

        inline void Flush()
        {
            auto& buffer = LogBuffer();
            if (LogFile() && !buffer.empty())
            {
                fwrite(buffer.data(), 1, buffer.size(), LogFile());
                fflush(LogFile());
            }
            buffer.clear();
        }

        inline bool Enabled()
        {
            // 옵션은 실행 중 변하지 않으므로 처음 한 번만 읽고, 종료 시 남은 로그를 쓰도록 등록한다.
            static const bool enabled = []
            {
                std::wstring path;
                if (!CommandLineArgs::GetString(L"cut-perf-log", path) || path.empty())
                    return false;
                if (_wfopen_s(&LogFile(), path.c_str(), L"w") != 0 || !LogFile())
                    return false;
                // 버퍼를 먼저 만들어야 종료 시 등록한 Flush보다 늦게 소멸한다.
                LogBuffer().reserve(1u << 20);
                std::atexit([] { Flush(); });
                return true;
            }();
            return enabled;
        }

        // 정수 옵션 값을 읽는다. 옵션이 없으면 fallback을 반환한다.
        inline uint32_t Value(const wchar_t* key, uint32_t fallback)
        {
            uint32_t value = fallback;
            CommandLineArgs::GetInteger(key, value);
            return value;
        }

        // PatchLog 파일 기록을 끄는 옵션이다. 측정 옵션이 켜진 경우에만 유효하다.
        inline bool Quiet()
        {
            static const bool quiet = Enabled() && Value(L"cut-perf-quiet", 0) != 0;
            return quiet;
        }

        // GPU timestamp 쿼리와 프로파일 블록을 만들지 않는 CPU 전용 모드를 고르는 옵션이다.
        inline bool GpuEnabled()
        {
            static const bool gpu = Enabled() && Value(L"cut-perf-gpu", 1) != 0;
            return gpu;
        }

        inline double NowMs()
        {
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // 한 줄을 메모리 버퍼에 쌓는다.
        inline void Log(const char* format, ...)
        {
            if (!Enabled()) return;
            char line[1024];
            va_list args;
            va_start(args, format);
            vsnprintf(line, sizeof(line), format, args);
            va_end(args);
            auto& buffer = LogBuffer();
            buffer += line;
            if (buffer.size() > (256u << 10)) Flush();
        }

        // 렌더에 쓰는 어댑터 이름과 전용 비디오 메모리, 켜진 옵션을 한 번 남긴다.
        inline void LogEnvironment()
        {
            if (!Enabled()) return;
            Microsoft::WRL::ComPtr<IDXGIFactory4> factory;
            Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))
                || FAILED(factory->EnumAdapterByLuid(Graphics::g_Device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))))
                return;
            DXGI_ADAPTER_DESC1 desc = {};
            adapter->GetDesc1(&desc);
            Log("ENV adapter=%ls dedicatedVideoMiB=%llu sharedSystemMiB=%llu vendor=%04x device=%04x revision=%u\n", desc.Description,
                uint64_t(desc.DedicatedVideoMemory >> 20), uint64_t(desc.SharedSystemMemory >> 20),
                desc.VendorId, desc.DeviceId, desc.Revision);
            Log("OPTIONS quiet=%d gpu=%d onlyTarget=%u focus=%u\n", Quiet() ? 1 : 0, GpuEnabled() ? 1 : 0,
                Value(L"cut-perf-only-target", 0), Value(L"cut-perf-focus", 0));
        }

        // 요청 시작 시각·프레임 번호와 요청 번호다. 모든 구간 시각은 이 시작점에서 잰다.
        inline double& RequestMs() { static double value = 0; return value; }
        inline uint64_t& RequestFrame() { static uint64_t value = 0; return value; }
        inline unsigned& RequestId() { static unsigned value = 0; return value; }

        inline void BeginRequest()
        {
            if (!Enabled()) return;
            RequestMs() = NowMs();
            RequestFrame() = Graphics::GetFrameCount();
            ++RequestId();
            Log("REQUEST id=%u frame=%llu\n", RequestId(), RequestFrame());
        }

        // 요청 이후 경과 ms와 프레임 수로 단계 도달 시각을 남긴다.
        inline void Mark(const char* stage)
        {
            if (!Enabled()) return;
            Log("MARK id=%u stage=%s t_ms=%.3f dframe=%llu\n", RequestId(), stage,
                NowMs() - RequestMs(), Graphics::GetFrameCount() - RequestFrame());
        }

        // 범위의 CPU 벽시계 시간(ms)을 범위 종료 시 남긴다.
        struct Cpu
        {
            const char* Name;
            double Start = Enabled() ? NowMs() : 0.0;
            explicit Cpu(const char* name) : Name(name) {}
            bool Active = Enabled();

            // 지역 변수의 수명은 유지하면서 세부 측정 구간만 한 번 종료한다.
            void End()
            {
                // 비활성 구간은 시계를 읽거나 로그를 남기지 않는다.
                if (!Active) return;
                Active = false;
                const double end = NowMs();
                Log("CPU id=%u name=%s ms=%.3f t_end_ms=%.3f\n", RequestId(), Name, end - Start, end - RequestMs());
            }

            // 조기 반환이나 예외에서도 아직 열린 측정 구간을 기록한다.
            ~Cpu()
            {
                // 명시적으로 종료한 구간은 다시 기록하지 않는다.
                End();
            }
        };

        // 프레임 함수의 CPU 시간 칸이다. Update와 RenderScene이 각자 한 칸씩 쓴다.
        inline double& FrameCpuMs(int slot) { static double value[2] = {}; return value[slot]; }
        struct FrameCpu
        {
            int Slot;
            double Start = Enabled() ? NowMs() : 0.0;
            explicit FrameCpu(int slot) : Slot(slot) {}
            ~FrameCpu() { if (Enabled()) FrameCpuMs(Slot) = NowMs() - Start; }
        };

        // GPU timestamp 한 쌍이다. 완료 fence가 끝난 뒤 Poll에서 읽어 로그에 남긴다.
        struct Query
        {
            Microsoft::WRL::ComPtr<ID3D12QueryHeap> Heap;
            ReadbackBuffer Readback;
            std::string Name;
            uint64_t Frequency = 0, Fence = 0;
            unsigned Request = 0;
        };
        inline std::vector<std::shared_ptr<Query>>& PendingQueries()
        {
            static std::vector<std::shared_ptr<Query>> value;
            return value;
        }

        inline void Poll()
        {
            if (!GpuEnabled()) return;
            auto& pending = PendingQueries();
            for (auto it = pending.begin(); it != pending.end();)
            {
                auto& q = **it;
                if (!Graphics::g_CommandManager.IsFenceComplete(q.Fence)) { ++it; continue; }
                const auto* ticks = static_cast<const uint64_t*>(q.Readback.Map());
                const double us = double(ticks[1] - ticks[0]) * 1e6 / double(q.Frequency);
                q.Readback.Unmap();
                Log("GPU id=%u name=%s us=%.3f\n", q.Request, q.Name.c_str(), us);
                it = pending.erase(it);
            }
        }

        // 명령 기록 구간을 timestamp로 감싼다. 시작 직전 barrier를 flush하므로 barrier는 구간 밖이다.
        struct Gpu
        {
            CommandContext& Context;
            std::shared_ptr<Query> Q;
            Gpu(CommandContext& context, const char* name) : Context(context)
            {
                if (!GpuEnabled()) return;
                Q = std::make_shared<Query>();
                Q->Name = name;
                Q->Request = RequestId();
                D3D12_QUERY_HEAP_DESC desc = {};
                desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
                desc.Count = 2;
                ASSERT_SUCCEEDED(Graphics::g_Device->CreateQueryHeap(&desc, IID_PPV_ARGS(&Q->Heap)));
                ASSERT_SUCCEEDED(Graphics::g_CommandManager.GetCommandQueue()->GetTimestampFrequency(&Q->Frequency));
                Q->Readback.Create(L"CutPerf timestamp", 2, 8);
                Context.FlushResourceBarriers();
                Context.InsertTimeStamp(Q->Heap.Get(), 0);
            }
            void End()
            {
                if (!Q) return;
                Context.InsertTimeStamp(Q->Heap.Get(), 1);
                Context.ResolveTimeStamps(Q->Readback.GetResource(), Q->Heap.Get(), 2);
                Q->Fence = Graphics::g_CommandManager.GetGraphicsQueue().GetNextFenceValue();
                PendingQueries().push_back(std::move(Q));
            }
            ~Gpu() { End(); }
        };

        // EngineProfiling 블록(GPU 타이머 포함)을 Release에서도 켠다. 프레임별 CPU·GPU 시간 수집용이다.
        struct Block
        {
            CommandContext* Context = nullptr;
            bool Active = false;
            Block(const wchar_t* name, CommandContext& context)
            {
                if (!GpuEnabled()) return;
                Context = &context;
                Active = true;
                EngineProfiling::BeginBlock(name, Context);
            }
            void End()
            {
                if (!Active) return;
                Active = false;
                EngineProfiling::EndBlock(Context);
            }
            ~Block() { End(); }
        };

        // 버퍼의 논리 크기와 실제 할당 크기를 남긴다.
        inline void Memory(const char* scope, const char* name, const GpuBuffer& buffer)
        {
            if (!Enabled()) return;
            auto* resource = const_cast<ID3D12Resource*>(buffer.GetResource());
            if (!resource) return;
            const auto desc = resource->GetDesc();
            const auto info = Graphics::g_Device->GetResourceAllocationInfo(0, 1, &desc);
            Log("MEM id=%u scope=%s name=%s logical=%llu allocated=%llu\n", RequestId(), scope, name,
                uint64_t(buffer.GetBufferSize()), uint64_t(info.SizeInBytes));
        }

        // 직전 프레임의 한 줄을 남긴다. 블록 값은 EngineProfiling이 이번 프레임 시작에 확정한 직전 프레임 값이다.
        inline void LogPreviousFrame(double wallMs, double patchSkinCpuMs, double patchSkinGpuMs, unsigned patchSkinPieces)
        {
            const wchar_t* names[] = { L"CP Scene Update", L"CP Render", L"CP Depth", L"CP Shadow", L"CP Color" };
            float cpu[5] = {}, gpu[5] = {};
            bool ok[5] = {};
            if (GpuEnabled())
                for (int i = 0; i < 5; ++i)
                    ok[i] = EngineProfiling::GetLastBlockTimes(names[i], cpu[i], gpu[i]);
            Log("FRAME f=%llu wall_ms=%.3f cpuUpdate_ms=%.3f cpuRender_ms=%.3f "
                "upd_gpu=%.3f ren_gpu=%.3f depth_gpu=%.3f shadow_gpu=%.3f color_gpu=%.3f "
                "ok=%d%d%d%d%d skin_cpu=%.3f skin_gpu=%.3f skin_pieces=%u\n",
                Graphics::GetFrameCount() - 1, wallMs, FrameCpuMs(0), FrameCpuMs(1),
                gpu[0], gpu[1], gpu[2], gpu[3], gpu[4],
                ok[0], ok[1], ok[2], ok[3], ok[4], patchSkinCpuMs, patchSkinGpuMs, patchSkinPieces);
        }

        // Update 시작에서 직전 프레임 행을 쓰고 완료된 GPU 쿼리를 회수한다. 이 함수는 Update CPU 측정 밖에서 호출한다.
        // 대상 외 오브젝트 숨김(-cut-perf-only-target 1)도 여기서 3프레임째에 한 번 적용한다.
        template <class SceneT>
        inline void FrameBegin(SceneT& scene, uint32_t queryFrame, const char* targetName)
        {
            if (!Enabled()) return;

            // 직전 프레임 간격과 조각 스키닝 블록 시간을 모아 한 줄로 남긴다.
            static double previousUpdateMs = 0.0;
            const double nowMs = NowMs();
            double skinCpu = 0.0, skinGpu = 0.0;
            unsigned skinPieces = 0;
            if (GpuEnabled())
                for (const auto& object : scene.GetObjects())
                    for (const auto& component : object->GetMeshComponents())
                    {
                        float cpuMs = 0.0f, gpuMs = 0.0f;
                        if (component->HasSkinnedPatch() && !component->PatchSkinTimingName.empty()
                            && EngineProfiling::GetLastBlockTimes(component->PatchSkinTimingName, cpuMs, gpuMs))
                        {
                            skinCpu += cpuMs;
                            skinGpu += gpuMs;
                            ++skinPieces;
                        }
                    }
            if (previousUpdateMs == 0.0) LogEnvironment();
            else LogPreviousFrame(nowMs - previousUpdateMs, skinCpu, skinGpu, skinPieces);
            previousUpdateMs = nowMs;
            Poll();

            // 대상 외 오브젝트는 갱신은 그대로 두고 그리기만 건너뛴다.
            if (queryFrame == 3 && targetName && Value(L"cut-perf-only-target", 0) == 1)
                for (const auto& object : scene.GetObjects())
                    if (object->GetName() != targetName)
                        for (const auto& component : object->GetMeshComponents())
                            component->PublishVisible = false;
        }
    }
}
