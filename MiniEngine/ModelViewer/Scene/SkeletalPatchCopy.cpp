#include "SkeletalPatchCopy.h"
#include "CutPerfLog.h"
#include "CommandListManager.h"
#include "GraphicsCore.h"
#include "EngineProfiling.h"
#include "Utility.h"
#include "ObjectSelection.h"
#include "CuttingSystem.h"
#include "SkeletalAutoRig.h"
#include "Util/CommandLineArg.h"
#include "Renderer.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "CompiledShaders/PatchExtractCS.h"
#include "CompiledShaders/IslandDistanceCS.h"
#include "CompiledShaders/IslandHookCS.h"
#include "CompiledShaders/IslandJumpCS.h"
#include "CompiledShaders/IslandVerifyCS.h"
#include "CompiledShaders/IslandNumberCS.h"
#include "CompiledShaders/IslandVoteCS.h"
#include "CompiledShaders/PatchBodyClassifyCS.h"
#include "CompiledShaders/PatchBodyClassifyGpuCS.h"
#include "CompiledShaders/PatchBodyCompactCS.h"
#include "tests/SkeletalPatchReference.h"
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <exception>
#include <set>
#include <map>
#include <array>
#include <numeric>
#include <algorithm>
#include <tuple>
#include <sstream>
#include <chrono>
#include <unordered_map>
#include "SystemTime.h"
#include "../../Model/json.hpp"

namespace Viewer
{
    namespace
    {
        RootSignature PatchRoot;
        ComputePSO PatchPSO(L"Extract skeletal patch");
        RootSignature UnionRoot;
        ComputePSO HookPSO(L"Island hook"), JumpPSO(L"Island jump"), VerifyPSO(L"Island verify");
        ComputePSO NumberPSO(L"Island number");
        RootSignature VoteRoot;
        ComputePSO VotePSO(L"Island vote");
        RootSignature DistanceRoot;
        ComputePSO DistancePSO(L"Island corner distances");
        struct alignas(16) DistanceParameters
        {
            Math::Vector4 Plane;
            Math::Matrix4 World;
            uint32_t Section, Draws, Capacity, Padding;
        };
        static_assert(sizeof(DistanceParameters) == 96, "Island distance layout");
        struct DrawInput
        {
            uint32_t VbOffset, Stride, IbOffset, Index32;
            uint32_t StartIndex, BaseVertex, Triangles, PoseOffset;
            uint32_t FirstTriangle, MaskOffset, Flags, SectionId;
            uint32_t DrawId, JointCount, VertexCount, WordOffset;
        };
        struct alignas(16) Parameters
        {
            Math::Vector4 Plane;
            Math::Matrix4 LocalToRectangle;
            Math::Matrix4 LocalToWorld;
            float Epsilon, HalfWidth, HalfHeight;
            uint32_t Triangles, Draws, DepthOffset, IndexOffset, Mode;
            uint32_t NodeCount, Influence, HasState, BitDraw;
        };
        static_assert(sizeof(DrawInput) == 64 && sizeof(Parameters) == 192, "Patch shader layout");
        static_assert(sizeof(SkeletalPatchResult) == 4, "Patch counts readback layout");

        // GPU AABB의 정렬 가능한 정수 비트를 원래 float 좌표로 복원한다.
        float BoundsCoordinate(uint32_t ordered)
        {
            // 양수와 음수에 적용했던 비트 변환을 각각 되돌린다.
            const uint32_t bits = ordered & 0x80000000u ? ordered ^ 0x80000000u : ~ordered;
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        }

        // skeletonRoot에서 분리된 본 pose에 렌더와 같은 mesh node 및 source 보정을 적용한다.
        Math::Matrix4 JointFrameToWorld(const ModelInstance& pose, const MeshComponent& component,
            const SceneTransform& owner, size_t section)
        {
            // PreparePoseCS의 NodeMatrix와 VS의 component world를 순서대로 합성한다.
            return Math::Matrix4(component.GetWorldTransform(owner))
                * Invert(Math::Matrix4(component.GetDeformBinding().SourceFrame.ToAffine()))
                * pose.GetNodeTransforms()[component.GetGeometry()->GetMesh(section).meshCBV];
        }

        // Release 실행에서도 확인할 수 있도록 추출 로그를 파일과 debugger에 남긴다.
        void PatchLog(const char* format, ...)
        {
            // 성능 측정의 quiet 모드에서는 파일 기록을 하지 않는다.
            if (CutPerf::Quiet()) return;

            // 가변 인자를 한 번 포맷하고 동일한 문자열을 두 출력에 쓴다.
            char line[1024];
            va_list args;
            va_start(args, format);
            vsnprintf(line, sizeof(line), format, args);
            va_end(args);
            Utility::Print(line);
            FILE* file = nullptr;
            char path[MAX_PATH];
            if (!GetTempPathA(MAX_PATH, path)) throw std::runtime_error("No patch log directory");
            strcat_s(path, "SP6-SkeletalPatchChecks.log");
            if (fopen_s(&file, path, "a") == 0)
            {
                fputs(line, file);
                fclose(file);
            }

            // 최종 비교 행만 별도 파일에 모아 두 모드를 나란히 검토한다.
            if (std::strncmp(line, "SP6 comparison", 14) == 0)
            {
                GetTempPathA(MAX_PATH, path);
                strcat_s(path, "SP6-comparison.log");
                if (fopen_s(&file, path, "a") == 0)
                {
                    fputs(line, file);
                    fclose(file);
                }
            }
        }

        // 두 side 합성 출력으로 다중 몸체와 빈 마지막 몸체의 압축 계약을 검사한다.
        void CheckBodyCompaction()
        {
            // 기존 검사 옵션에서 한 번만 실행하며 게시 경로에는 연결하지 않는다.
            static bool checked = false;
            uint32_t checks = 0;
            CommandLineArgs::GetInteger(L"skeletal-patch-break-checks", checks);
            uint32_t unionChecks = 0;
            CommandLineArgs::GetInteger(L"skeletal-union-checks", unionChecks);
            checks |= unionChecks;
            if (!checks || checked) return;
            checked = true;
            struct alignas(16) Constants
            {
                uint32_t Mode, BodyCount, Sections, Section;
                uint32_t FirstTriangle, Triangles, VbOffset, DepthOffset;
                uint32_t IbOffset, Stride, DepthStride, CapacityBytes;
                uint32_t CapacityTriangles, Padding[3];
            };
            const uint32_t layouts[] = { 28, 16, 28, 0, 28, 12 }; // section 2는 빈 구간
            uint32_t input[2][500]{}, provenance[2][192]{}, bodies[2][8]{};
            for (uint32_t side = 0; side < 2; ++side)
                for (uint32_t t : { 0u, 1u, 4u })
                {
                    bodies[side][t] = (side + t) % 2;
                    for (uint32_t c = 0; c < 3; ++c)
                    {
                        const uint32_t token = side * 1000 + t * 10 + c + 1;
                        for (uint32_t w = 0; w < 7; ++w) input[side][t * 21 + c * 7 + w] = token * 100 + w;
                        for (uint32_t w = 0; w < 4; ++w) input[side][250 + t * 12 + c * 4 + w] = token * 100 + 20 + w;
                        for (uint32_t w = 0; w < 8; ++w) provenance[side][t * 24 + c * 8 + w] = token * 100 + 40 + w;
                        if (t == 4) provenance[side][t * 24 + c * 8] = UINT32_MAX;
                        input[side][350 + t * 3 + c] = (t == 4 ? 0 : t * 3) + (c + 2) % 3;
                    }
                }

            // 실제 셰이더에 RAW 버퍼를 바인딩하고 출력 전체를 진단용으로만 읽는다.
            ByteAddressBuffer data[2], prov[2], labels[2], sections, output, outProv, bins, status;
            for (uint32_t side = 0; side < 2; ++side)
            {
                data[side].Create(L"Compact synthetic data", 500, 4, input[side]);
                prov[side].Create(L"Compact synthetic provenance", 192, 4, provenance[side]);
                labels[side].Create(L"Compact synthetic bodies", 8, 4, bodies[side]);
            }
            sections.Create(L"Compact layouts", 6, 4, layouts);
            output.Create(L"Compact output", 512, 4);
            outProv.Create(L"Compact provenance", 144, 4);
            bins.Create(L"Compact bins", 72, 4);
            status.Create(L"Compact status", 1, 4);
            ReadbackBuffer readData, readProv, readBins, readStatus;
            readData.Create(L"Compact data readback", 512, 4);
            readProv.Create(L"Compact provenance readback", 144, 4);
            readBins.Create(L"Compact bins readback", 72, 4);
            readStatus.Create(L"Compact status readback", 1, 4);
            RootSignature root;
            root.Reset(9);
            root[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 4; ++i) root[i + 1].InitAsBufferSRV(i);
            for (uint32_t i = 0; i < 4; ++i) root[i + 5].InitAsBufferUAV(i);
            root.Finalize(L"Body compact checks");
            ComputePSO pso(L"Body compact checks");
            pso.SetRootSignature(root);
            pso.SetComputeShader(g_pPatchBodyCompactCS, sizeof(g_pPatchBodyCompactCS));
            pso.Finalize();
            bool pass = true;

            // 정상·용량 부족·drop 검사 뒤 같은 side의 B1/B2 압축과 빈 B3를 검사한다.
            for (uint32_t trial = 0; trial < 5; ++trial)
            {
                // 네 번째 실행은 cap만 제거하고 표면의 몸체 귀속은 그대로 유지한다.
                if (trial == 3)
                {
                    for (uint32_t side = 0; side < 2; ++side)
                    {
                        bodies[side][4] = UINT32_MAX - 1;
                        labels[side].Create(L"Compact synthetic dropped caps", 8, 4, bodies[side]);
                    }
                }

                // 세 번째 몸체에는 side0 cap만 보내며 네 번째 몸체는 의도적으로 비운다.
                if (trial == 4)
                {
                    bodies[0][4] = 2;
                    bodies[1][4] = 1;
                    for (uint32_t side = 0; side < 2; ++side)
                    {
                        labels[side].Create(L"Compact synthetic multiple bodies", 8, 4, bodies[side]);
                    }
                }

                // 각 실행의 출력과 진단 상태를 초기화한다.
                auto& graphics = GraphicsContext::Begin(L"Body compact checks");
                auto& compute = graphics.GetComputeContext();
                for (auto* buffer : { &output, &outProv, &bins, &status })
                {
                    compute.TransitionResource(*buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    compute.ClearUAV(*buffer);
                    compute.InsertUAVBarrier(*buffer);
                }
                compute.TransitionResource(sections, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                compute.SetRootSignature(root);
                compute.SetPipelineState(pso);
                compute.SetBufferSRV(4, sections);
                compute.SetBufferUAV(5, output);
                compute.SetBufferUAV(6, outProv);
                compute.SetBufferUAV(7, bins);
                compute.SetBufferUAV(8, status);
                Constants constants{ 0, 4, 3, 0, 0, 0, 0, 1000, 0, 28, 0, trial == 2 ? 1u : 2048u,
                    trial == 1 ? 5u : 6u, {} };
                for (uint32_t mode = 0; mode < 3; ++mode)
                {
                    constants.Mode = mode;
                    for (uint32_t side = 0; side < (mode == 1 ? 1u : 2u); ++side)
                    {
                        for (auto* buffer : { &data[side], &prov[side], &labels[side] })
                            compute.TransitionResource(*buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                        compute.SetBufferSRV(1, data[side]);
                        compute.SetBufferSRV(2, prov[side]);
                        compute.SetBufferSRV(3, labels[side]);
                        for (uint32_t section = 0; section < (mode == 1 ? 1u : 3u); ++section)
                        {
                            constants.Section = section;
                            constants.FirstTriangle = section ? 4 : 0;
                            constants.Triangles = section == 2 ? 0 : section ? 1 : 2;
                            constants.VbOffset = constants.FirstTriangle * 84;
                            constants.DepthStride = layouts[section * 2 + 1];
                            constants.IbOffset = 1400 + constants.FirstTriangle * 12;
                            compute.SetDynamicConstantBufferView(0, sizeof(constants), &constants);
                            compute.Dispatch(1, 1, 1);
                        }
                    }
                    compute.InsertUAVBarrier(bins);
                    compute.InsertUAVBarrier(status);
                }
                graphics.CopyBuffer(readData, output);
                graphics.CopyBuffer(readProv, outProv);
                graphics.CopyBuffer(readBins, bins);
                graphics.CopyBuffer(readStatus, status);
                graphics.Finish(true);
                const auto* result = static_cast<const uint32_t*>(readData.Map());
                const auto* sources = static_cast<const uint32_t*>(readProv.Map());
                const auto* meta = static_cast<const uint32_t*>(readBins.Map());
                const uint32_t flags = *static_cast<const uint32_t*>(readStatus.Map());
                const bool overflow = trial == 1 || trial == 2;
                pass &= flags == (overflow ? 1u : 0u);

                // 공통 정점 주소와 모든 코너·depth·출처·IB 및 몸체별 합계를 비교한다.
                bool seen[2][8]{};
                uint32_t total = 0;
                for (uint32_t bin = 0; bin < 12 && !overflow; ++bin)
                {
                    const uint32_t* m = meta + bin * 6;
                    pass &= m[2] % 28 == 0 && m[2] / 28 == m[1] * 3;
                    uint32_t expected = 0;
                    for (uint32_t side = 0; side < 2; ++side)
                        for (uint32_t triangle : { 0u, 1u, 4u })
                            if (bodies[side][triangle] == bin / 3 && (triangle == 4 ? 1u : 0u) == bin % 3)
                                ++expected;
                    pass &= m[0] == expected;
                    total += m[0];
                    for (uint32_t t = 0; t < m[0]; ++t)
                    {
                        const uint32_t token = result[m[2] / 4 + t * 21] / 100;
                        const uint32_t side = token / 1000, triangle = (token % 1000) / 10;
                        if (side >= 2 || (triangle != 0 && triangle != 1 && triangle != 4))
                        {
                            pass = false;
                            continue;
                        }
                        pass &= !seen[side][triangle] && bodies[side][triangle] == bin / 3 && (triangle == 4 ? 1u : 0u) == bin % 3;
                        seen[side][triangle] = true;
                        for (uint32_t c = 0; c < 3; ++c)
                        {
                            const uint32_t source = triangle * 3 + (c + 2) % 3, v = t * 3 + c;
                            pass &= std::memcmp(result + m[2] / 4 + v * 7, input[side] + source * 7, 28) == 0;
                            if (bin % 3 == 0) pass &= std::memcmp(result + m[3] / 4 + v * 4, input[side] + 250 + source * 4, 16) == 0;
                            pass &= std::memcmp(sources + (m[1] * 3 + v) * 8, provenance[side] + source * 8, 32) == 0;
                            pass &= result[m[4] / 4 + v] == v;
                        }
                    }
                }
                if (!overflow) pass &= total == (trial == 3 ? 4u : 6u);
                else for (uint32_t w = 0; w < 512; ++w) pass &= result[w] == 0;

                // 마지막 빈 bin과 세 몸체의 실제 합계를 별도로 검사하고 기록한다.
                if (trial == 4)
                {
                    const uint32_t totals[] = { meta[0] + meta[6], meta[18] + meta[24], meta[36] + meta[42] };
                    pass &= totals[0] == 2 && totals[1] == 3 && totals[2] == 1;
                    PatchLog("MI4d synthetic body0=%u body1=%u body2=%u empty body=3 tris=%u sameSideBodies=1,2 %s\n",
                        totals[0], totals[1], totals[2], meta[54] + meta[60] + meta[66], pass ? "PASS" : "FAIL");
                }
                readData.Unmap();
                readProv.Unmap();
                readBins.Unmap();
                readStatus.Unmap();
            }
            PatchLog("MI3b synthetic two-side vertices/provenance/depth/IB skinAddress=matched totals=6 dropCaps=2 output=4 overflow=triangles+bytes %s\n", pass ? "PASS" : "FAIL");
            if (!pass)
                throw std::runtime_error("MI4d synthetic body compaction failed");
        }

        // 패치 진단의 문자열 생성과 출력을 요청의 예외 경계에서 분리한다.
        template<typename BuildText>
        void WritePatchLogSafely(const BuildText& buildText) noexcept
        {
            // quiet 모드에서는 로그 문자열도 만들지 않는다.
            if (CutPerf::Quiet()) return;

            // 기존 패치 로그 경로를 유지하며 진단 실패만 무시한다.
            try
            {
                PatchLog("%s", buildText().c_str());
            }
            catch (...)
            {
                // 진단 예외가 실제 선택과 GPU 요청을 중단하지 않게 한다.
            }
        }

        struct DiagnosticNames
        {
            std::filesystem::file_time_type Modified;
            uintmax_t Size = 0;
            std::tuple<DWORD, DWORD, DWORD, LONGLONG> Revision;
            std::vector<std::string> Names;
            std::vector<std::exception_ptr> NameErrors;
            std::exception_ptr TraversalError;
            uint32_t CycleBegin = UINT32_MAX;
        };

        // GLB 이름을 캐시하며 조상 재방문은 기존 DFS의 반복 구간으로 조회하고, 다른 재방문은 그대로 순회한다.
        // 파일 ID·변경 시각도 비교하되 모든 메타데이터가 같은 내용 변경은 진단 이름에 반영되지 않을 수 있다.
        std::string DiagnosticNodeName(const std::string& model, uint32_t node)
        {
            // Model 주소를 쓰지 않아 해제 후 주소 재사용의 영향을 받지 않는다.
            // 같은 GLB 경로는 모델 수명과 무관하게 공유하며 파일 변경·삭제 시 폐기한다.
            static std::map<std::string, DiagnosticNames> cache;
            const std::string path = "Models/" + model + ".glb";
            std::error_code error;
            const auto modified = std::filesystem::last_write_time(path, error);
            const auto size = error ? uintmax_t(0) : std::filesystem::file_size(path, error);

            // 크기·수정 시각을 보존한 교체와 덮어쓰기도 파일 ID·변경 시각으로 구분한다.
            const HANDLE fileInfo = CreateFileA(path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            BY_HANDLE_FILE_INFORMATION identity{};
            FILE_BASIC_INFO basic{};
            const bool revisionKnown = fileInfo != INVALID_HANDLE_VALUE
                && GetFileInformationByHandle(fileInfo, &identity)
                && GetFileInformationByHandleEx(fileInfo, FileBasicInfo, &basic, sizeof(basic));
            if (fileInfo != INVALID_HANDLE_VALUE) CloseHandle(fileInfo);
            const auto revision = std::make_tuple(identity.dwVolumeSerialNumber,
                identity.nFileIndexHigh, identity.nFileIndexLow, basic.ChangeTime.QuadPart);

            // 메타데이터 조회 실패 시에도 이전 캐시를 재사용하지 않는다.
            auto found = cache.find(path);
            if (error || !revisionKnown || (found != cache.end()
                && (found->second.Modified != modified || found->second.Size != size
                    || found->second.Revision != revision)))
            {
                cache.erase(path);
                found = cache.end();
            }

            // 실패한 읽기는 저장하지 않아 파일 복구 후 기존처럼 다시 시도한다.
            if (found == cache.end())
            {
                std::ifstream file(path, std::ios::binary);
                uint32_t header[5]{};
                file.read(reinterpret_cast<char*>(header), sizeof(header));
                if (!file || header[0] != 0x46546c67 || header[4] != 0x4e4f534a) return "unavailable";
                std::string bytes(header[3], ' ');
                file.read(&bytes[0], bytes.size());
                const auto json = nlohmann::json::parse(bytes);
                const auto roots = json["scenes"][json.value("scene", 0)]["nodes"].get<std::vector<uint32_t>>();
                std::vector<std::pair<uint32_t, bool>> stack;
                for (auto root = roots.rbegin(); root != roots.rend(); ++root)
                    stack.emplace_back(*root, false);
                std::map<uint32_t, uint32_t> visiting;
                DiagnosticNames names;
                names.Modified = modified;
                names.Size = size;
                names.Revision = revision;

                // 기존 전위 순서와 unnamed 기본값을 유지하고 노드별 이름 오류를 보존한다.
                while (!stack.empty())
                {
                    const auto entry = stack.back();
                    stack.pop_back();
                    const auto current = entry.first;
                    if (entry.second)
                    {
                        visiting.erase(current);
                        continue;
                    }

                    // 조상 재진입 뒤에는 같은 전위 구간이 무한 반복되므로 한 번만 저장한다.
                    const auto visit = visiting.emplace(current, uint32_t(names.Names.size()));
                    if (!visit.second)
                    {
                        names.CycleBegin = visit.first->second;
                        break;
                    }
                    stack.emplace_back(current, true);

                    // 이름 오류는 해당 NodeId 조회에만 적용한다.
                    names.Names.emplace_back();
                    names.NameErrors.emplace_back();
                    try
                    {
                        names.Names.back() = json["nodes"][current].value("name", std::string("unnamed"));
                    }
                    catch (...)
                    {
                        names.NameErrors.back() = std::current_exception();
                    }

                    // 원래 조회는 현재 이름을 먼저 반환하므로 자식 오류는 다음 NodeId부터 적용한다.
                    try
                    {
                        const auto children = json["nodes"][current].value("children", std::vector<uint32_t>{});
                        for (auto child = children.rbegin(); child != children.rend(); ++child)
                            stack.emplace_back(*child, false);
                    }
                    catch (...)
                    {
                        names.TraversalError = std::current_exception();
                        break;
                    }
                }
                found = cache.emplace(path, std::move(names)).first;
            }

            // Safe 호출자의 번호 대체와 직접 호출자의 예외 전파를 그대로 유지한다.
            const auto& names = found->second;
            if (names.CycleBegin != UINT32_MAX && node >= names.CycleBegin)
                node = names.CycleBegin + (node - names.CycleBegin) % (names.Names.size() - names.CycleBegin);
            if (node < names.Names.size())
            {
                if (names.NameErrors[node]) std::rethrow_exception(names.NameErrors[node]);
                return names.Names[node];
            }
            if (names.TraversalError) std::rethrow_exception(names.TraversalError);
            return "unavailable";
        }

        // 로그용 이름 조회가 실패하면 노드 번호로 대체한다.
        std::string SafeDiagnosticNodeName(const std::string& model, uint32_t node)
        {
            // 파일과 JSON 오류는 진단 이름에만 영향을 준다.
            try
            {
                const auto name = DiagnosticNodeName(model, node);
                if (name != "unavailable") return name;
            }
            catch (...)
            {
                // 번호 문자열 생성은 호출자의 안전 로그 경계에서 보호한다.
            }

            // 이름이 없거나 조회에 실패한 노드를 번호로 식별한다.
            return std::to_string(node);
        }

        // 자기 몸인 분리 subtree는 제외하고 기존 패치의 live weight와 현재 팔레트를 진단한다.
        void LogStaleSkin(const GameObject& owner, const SkeletalRigState& rig, uint64_t request)
        {
            // quiet 실행에서는 CPU 사본이나 팔레트를 읽지 않고 로그도 남기지 않는다.
            if (CutPerf::Quiet())
                return;

            // 패치가 공유하는 모델과 본 계층을 조회한다.
            const auto& pose = *owner.GetModelInstance();
            const auto& model = *pose.GetModel();
            const auto& parents = pose.GetPoseParents();

            // 부모 체인을 따라 특정 분리 root의 subtree 소속을 판정한다.
            const auto inside = [&parents](uint32_t node, uint32_t root)
            {
                // 잘못된 부착 노드와 root는 소속 없음으로 처리한다.
                for (; node < parents.size(); node = parents[node])
                    if (node == root)
                        return true;
                return false;
            };

            // 가장 가까운 부모 body가 Dynamic이 아닌 body만 분리 root로 모은다.
            std::map<uint32_t, MiniPhysics::BodyMotion> bodyMotions;
            for (const auto& body : rig.Bodies)
                bodyMotions.emplace(body.Node, body.Motion);
            std::vector<uint32_t> detachedRoots;
            for (const auto& body : rig.Bodies)
            {
                // body가 없는 중간 본을 지나 실제 부모 body의 운동 상태를 찾는다.
                if (body.Motion != MiniPhysics::BodyMotion::Dynamic || body.Node >= parents.size())
                    continue;
                uint32_t parent = parents[body.Node];
                while (parent < parents.size() && !bodyMotions.count(parent))
                    parent = parents[parent];
                const auto found = bodyMotions.find(parent);
                if (found == bodyMotions.end() || found->second != MiniPhysics::BodyMotion::Dynamic)
                    detachedRoots.push_back(body.Node);
            }

            // 활성 패치의 참조 정점을 세며 보간 끝점도 GPU와 같은 weight로 검사한다.
            for (const auto& component : owner.GetMeshComponents())
            {
                if (!component->PatchSkin)
                    continue;
                const auto& skin = *component->PatchSkin;
                const auto& cpu = skin.Vertices.DiagnosticSkin;

                // 레코드를 읽기 전에 stride와 모든 section의 하한·상한을 검증한다.
                const size_t stride = skin.Vertices.Stride;
                bool valid = stride >= ((skin.Vertices.Flags & kSkinExtFlag) ? kSkinExtInternal : 16u);
                valid = valid && cpu.size() % stride == 0 && !skin.Sections.empty();
                for (const auto& section : skin.Sections)
                {
                    // 나눗셈과 뺄셈으로 정점 끝 범위의 정수 넘침을 피한다.
                    const size_t available = valid ? cpu.size() / stride : 0;
                    valid = valid && section.FirstVertex <= available
                        && section.VertexCount <= available - section.FirstVertex;
                }
                const char* cpuStatus = cpu.empty() ? "unavailable" : valid ? "exact" : "invalid";

                // 비활성/Stale 및 원본 고정 패치는 패치별 제외 건수만 남긴다.
                const uint32_t inactive = !component->HasSkinnedPatch();
                const uint32_t sourceFrozen = skin.Snapshot.SourceFrozen;
                if (inactive || sourceFrozen)
                {
                    PatchLog("SKINSTALE patch=%llu cut=%llu skippedInactive=%u skippedSourceFrozen=%u cpu=%s\n",
                        component->GetId(), request, inactive, sourceFrozen, cpuStatus);
                    continue;
                }

                // CPU 사본이 불완전하거나 배치가 잘못되면 이 패치의 집계를 건너뛴다.
                if (cpu.empty() || !valid)
                {
                    PatchLog("SKINSTALE patch=%llu cut=%llu cpu=%s skippedInactive=0 skippedSourceFrozen=0\n",
                        component->GetId(), request, cpuStatus);
                    continue;
                }

                // 자기 몸을 따르는 subtree만 제외하고 나머지 분리 subtree는 계속 검사한다.
                std::vector<uint8_t> detached(parents.size(), 0);
                bool excluded = false;
                for (uint32_t root : detachedRoots)
                {
                    if (inside(skin.Snapshot.AttachNode, root))
                    {
                        excluded = true;
                        continue;
                    }
                    for (uint32_t node = 0; node < parents.size(); ++node)
                        detached[node] |= inside(node, root);
                }

                // 같은 프레임에 준비된 실제 위치·법선 팔레트 전체의 유한성을 검사한다.
                RecutCheckInput palette;
                bool paletteFinite = component->TryGetRecutCheckInput(palette);
                for (size_t k = 0; k < palette.Palette.size(); ++k)
                    for (const Math::Matrix4& matrix : { palette.Palette[k], Math::Matrix4(palette.NormalPalette[k]) })
                    {
                        DirectX::XMFLOAT4X4 values;
                        DirectX::XMStoreFloat4x4(&values, matrix);
                        for (const auto& row : values.m)
                            for (float value : row)
                                paletteFinite &= std::isfinite(value);
                    }

                // 참조 정점의 비고정 weight를 정점당 한 번 집계한다.
                uint32_t count = 0;
                float maximum = 0;
                std::set<uint32_t> bones;
                for (const auto& section : skin.Sections)
                    for (size_t vertex = section.FirstVertex; vertex < size_t(section.FirstVertex) + section.VertexCount; ++vertex)
                    {
                        // 일반 레코드 또는 확장 끝점 두 개를 읽고 정점당 한 번만 집계한다.
                        const size_t end = (vertex + 1) * stride;
                        uint32_t interpolation = 0;
                        float t = 0;
                        if (skin.Vertices.Flags & kSkinExtFlag)
                            std::memcpy(&interpolation, cpu.data() + end - kSkinExtInternal, 4);
                        if (interpolation)
                            std::memcpy(&t, cpu.data() + end - kSkinExtT, 4);
                        bool stale = false;
                        for (uint32_t endpoint = 0; endpoint < (interpolation ? 2u : 1u); ++endpoint)
                        {
                            // u16 weight를 정규화하고 보간 계수를 곱하며 강체 슬롯 K는 제외한다.
                            uint16_t record[8];
                            std::memcpy(record, cpu.data() + end - (interpolation ? 68 - 36 * endpoint : 16), sizeof(record));
                            const float sum = std::max(1u, uint32_t(record[4]) + record[5] + record[6] + record[7]);
                            for (uint32_t slot = 0; slot < 4; ++slot)
                            {
                                const uint32_t joint = record[slot];
                                const float weight = record[slot + 4] / sum * (interpolation ? (endpoint ? t : 1 - t) : 1);
                                if (weight <= 0 || joint >= skin.Snapshot.Frozen.size() || skin.Snapshot.Frozen[joint]
                                    || joint >= model.m_NumJoints || model.m_JointIndices[joint] >= detached.size()
                                    || !detached[model.m_JointIndices[joint]])
                                    continue;
                                stale = true;
                                maximum = std::max(maximum, weight);
                                bones.insert(model.m_JointIndices[joint]);
                            }
                        }
                        count += stale;
                    }

                // 검증된 CPU 사본의 집계 결과에서 본을 NodeId로 식별한다.
                std::string names;
                for (uint32_t bone : bones)
                    names += std::to_string(bone) + ",";
                PatchLog("SKINSTALE patch=%llu cut=%llu vertsWithDetachedLiveWeight=%u maxWeight=%g bones=%s cpu=%s excluded=%u skippedInactive=0 skippedSourceFrozen=0 paletteFinite=%u\n",
                    component->GetId(), request, count, maximum, names.empty() ? "none" : names.c_str(),
                    cpuStatus, unsigned(excluded), unsigned(paletteFinite));
            }
        }

        // -topology-island-viz 옵션을 한 번만 읽어 켜짐 여부를 돌려준다.
        bool TopologyIslandVizEnabled()
        {
            // 프로세스 수명 동안 같은 값을 쓴다.
            static const bool enabled = []
            {
                // 값이 0이거나 없으면 꺼진 상태이다.
                uint32_t value = 0;
                try
                {
                    CommandLineArgs::GetInteger(L"topology-island-viz", value);
                }
                catch (...)
                {
                    // 정수가 아니면 꺼진 상태로 둔다.
                    value = 0;
                }
                return value != 0;
            }();
            return enabled;
        }

        // 디버깅 씬용 수평 평면 높이 비율을 읽는다. -topology-island-viz-height 값이 있으면 그 값을,
        // -topology-island-viz 2이면 기본 비율을 쓰고, 둘 다 없으면 false를 돌려준다.
        bool TopologyIslandVizHeightRatio(float& ratio)
        {
            // 튜닝으로 정한 몸통 가운데 기본 비율이다.
            ratio = 0.5f;

            // 높이 옵션이 있으면 값을 읽고 읽지 못하면 기본 비율을 유지한다.
            std::wstring text;
            if (CommandLineArgs::GetString(L"topology-island-viz-height", text))
            {
                try
                {
                    ratio = std::stof(text);
                }
                catch (...)
                {
                    // 숫자가 아니면 기본 비율을 그대로 쓴다.
                }

                // 유한하지 않은 값은 기본 비율로 되돌리고 0~1로 제한한다.
                if (!std::isfinite(ratio))
                {
                    ratio = 0.5f;
                }
                ratio = std::min(std::max(ratio, 0.0f), 1.0f);
                return true;
            }

            // 시각화 값이 2이면 기본 비율의 수평 평면을 쓴다.
            uint32_t value = 0;
            try
            {
                CommandLineArgs::GetInteger(L"topology-island-viz", value);
            }
            catch (...)
            {
                // 정수가 아니면 기본 평면을 쓰지 않는다.
                value = 0;
            }
            return value == 2;
        }

        // 아일랜드 크기 순위에 대응하는 12색 팔레트 색을 돌려준다.
        Color TopologyIslandColor(uint32_t rank)
        {
            // 서로 뚜렷이 구별되는 색을 고정 순서로 두고 넘치면 되풀이한다.
            static const Color colors[12] = {
                Color(0.90f, 0.10f, 0.10f), Color(0.15f, 0.45f, 1.00f), Color(0.10f, 0.80f, 0.20f),
                Color(1.00f, 0.85f, 0.10f), Color(0.80f, 0.20f, 0.90f), Color(0.10f, 0.85f, 0.85f),
                Color(1.00f, 0.55f, 0.10f), Color(0.55f, 0.35f, 0.15f), Color(1.00f, 0.50f, 0.75f),
                Color(0.60f, 0.90f, 0.20f), Color(0.50f, 0.50f, 1.00f), Color(0.95f, 0.95f, 0.95f) };
            return colors[rank % 12];
        }

        // 정수 옵션을 읽고 없으면 기본값을 돌려준다.
        uint32_t TopologyIslandVizOption(const wchar_t* name, uint32_t fallback)
        {
            // 옵션이 없으면 읽기 전에 채운 기본값이 그대로 남는다.
            uint32_t value = fallback;
            try
            {
                CommandLineArgs::GetInteger(name, value);
            }
            catch (...)
            {
                // 정수가 아니면 기본값으로 되돌린다.
                value = fallback;
            }
            return value;
        }

        // -topology-island-viz-method 값을 한 번만 읽는다. 0은 메시 방식, 1은 NDC 셀 그래프 방식이다.
        uint32_t TopologyIslandVizMethod()
        {
            // 프로세스 수명 동안 같은 값을 쓴다.
            static const uint32_t method = TopologyIslandVizOption(L"topology-island-viz-method", 0) != 0 ? 1 : 0;
            return method;
        }

        // -topology-island-viz-neighbors 값을 한 번만 읽는다. 6이 아니면 26으로 본다.
        uint32_t TopologyIslandVizNeighbors()
        {
            // 프로세스 수명 동안 같은 값을 쓴다.
            static const uint32_t neighbors = TopologyIslandVizOption(L"topology-island-viz-neighbors", 26) == 6 ? 6 : 26;
            return neighbors;
        }

        // -topology-island-viz-draw 값을 한 번만 읽는다. 1이면 셀 그래프를 그린다.
        bool TopologyIslandVizDrawGraph()
        {
            // 프로세스 수명 동안 같은 값을 쓴다.
            static const bool draw = TopologyIslandVizOption(L"topology-island-viz-draw", 0) != 0;
            return draw;
        }

        // 단조 시계의 현재 시각을 밀리초로 돌려준다.
        double TopologyNowMs()
        {
            // MSVC steady_clock은 QueryPerformanceCounter 기반이다.
            return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        // 캐시된 셀 그래프와 그 그래프를 만들 때의 모델 규모를 함께 보관한다.
        struct CachedCellGraph
        {
            LatticeCellGraph Graph;
            size_t VertexCount = 0;
            size_t IndexCount = 0;
            float CellSize = 0.0f;
        };

        // 모델별 NDC 셀 그래프를 캐시하고 정점 수·인덱스 수·셀 크기가 달라지면 다시 만든다. 빌드 시간과 개수를 기록한다.
        const LatticeCellGraph& TopologyCellGraph(const void* key, const LatticeTopology& topology,
            const std::vector<SkinCpuSection>& sections)
        {
            // 현재 모델의 검증 값을 구한다.
            size_t vertexCount = 0;
            for (const auto& section : sections)
            {
                vertexCount += section.size();
            }
            size_t indexCount = 0;
            for (const auto& indices : topology.Indices)
            {
                indexCount += indices.size();
            }

            // 같은 모델이고 검증 값이 모두 같으면 캐시를 그대로 돌려준다.
            static std::map<const void*, CachedCellGraph> cache;
            const auto found = cache.find(key);
            if (found != cache.end() && found->second.VertexCount == vertexCount
                && found->second.IndexCount == indexCount && found->second.CellSize == topology.CellSize)
            {
                return found->second.Graph;
            }

            // 처음이거나 검증 값이 다르면 그래프를 만들고 시간을 잰다.
            const double start = TopologyNowMs();
            CachedCellGraph entry;
            entry.Graph = BuildLatticeCellGraph(topology);
            entry.VertexCount = vertexCount;
            entry.IndexCount = indexCount;
            entry.CellSize = topology.CellSize;
            const double elapsed = TopologyNowMs() - start;
            PatchLog("TIV cellgraph build ms=%.2f cells=%zu samples=%zu\n",
                elapsed, entry.Graph.CellCoords.size(), entry.Graph.SampleCell.size());
            cache[key] = std::move(entry);
            return cache[key].Graph;
        }

        // 셀 아일랜드 방식의 정점 아일랜드로 삼각형 간선을 색칠한다. 두 끝 아일랜드가 다르면 평면 교점이나 중점에서 나눈다.
        void AddCellIslandWireframe(std::vector<CollisionDebugDraw::WorldLine>& lines, const LatticeTopology& topology,
            const std::vector<std::vector<Math::Vector3>>& positions, const std::vector<std::vector<float>>& distances,
            const std::vector<std::vector<uint32_t>>& vertexIsland)
        {
            // 같은 간선을 두 삼각형이 공유하므로 한 번만 낸다.
            std::set<std::array<uint32_t, 3>> emitted;
            for (uint32_t skin = 0; skin < positions.size(); ++skin)
            {
                if (positions[skin].empty())
                {
                    continue;
                }
                const auto& indices = topology.Indices[skin];
                for (size_t i = 0; i + 2 < indices.size(); i += 3)
                {
                    for (uint32_t e = 0; e < 3; ++e)
                    {
                        // 간선 끝점 두 개를 읽고 중복 간선을 건너뛴다.
                        const uint32_t a = indices[i + e];
                        const uint32_t b = indices[i + (e + 1) % 3];
                        if (!emitted.insert({ skin, std::min(a, b), std::max(a, b) }).second)
                        {
                            continue;
                        }

                        // 두 끝이 같은 아일랜드면 통째로 그 색이다.
                        const uint32_t islandA = vertexIsland[skin][a];
                        const uint32_t islandB = vertexIsland[skin][b];
                        if (islandA == islandB)
                        {
                            lines.push_back({ positions[skin][a], positions[skin][b], TopologyIslandColor(islandA) });
                            continue;
                        }

                        // 평면을 가로지르면 교점에서, 아니면 중점에서 나눈다.
                        const float da = distances[skin][a];
                        const float db = distances[skin][b];
                        float fraction = 0.5f;
                        if (da * db < 0.0f)
                        {
                            fraction = da / (da - db);
                        }
                        const Math::Vector3 split = positions[skin][a] + (positions[skin][b] - positions[skin][a]) * fraction;
                        lines.push_back({ positions[skin][a], split, TopologyIslandColor(islandA) });
                        lines.push_back({ split, positions[skin][b], TopologyIslandColor(islandB) });
                    }
                }
            }
        }

        // 반대 방향 중복을 피하려고 사전식으로 양인 오프셋만 허용하고, 6-이웃이면 면 이웃만 허용한다.
        bool IsForwardNeighborOffset(int dx, int dy, int dz, uint32_t neighborhood)
        {
            // 사전식으로 양인 오프셋인지 판정한다.
            const bool forward = dx > 0 || (dx == 0 && dy > 0) || (dx == 0 && dy == 0 && dz > 0);

            // 6-이웃이면 0이 아닌 축이 하나인 오프셋만 남긴다.
            const int nonZero = (dx != 0) + (dy != 0) + (dz != 0);
            return forward && (neighborhood != 6 || nonZero == 1);
        }

        // 포즈된 표본 평균으로 셀 중심을 구해 십자와 유지된 인접 간선을 아일랜드 색으로 그린다.
        void AddCellGraphLines(std::vector<CollisionDebugDraw::WorldLine>& lines, const LatticeCellGraph& graph,
            const std::vector<std::vector<Math::Vector3>>& positions, const std::vector<int8_t>& cellSides,
            const CellIslandLabels& labels, uint32_t neighborhood, float crossHalf)
        {
            // 표본의 포즈 위치를 정점 위치의 무게중심 보간으로 구해 셀별로 누적한다.
            const size_t cellCount = graph.CellCoords.size();
            std::vector<Math::Vector3> centers(cellCount, Math::Vector3(Math::kZero));
            std::vector<uint32_t> counts(cellCount, 0);
            for (size_t k = 0; k < graph.SampleCell.size(); ++k)
            {
                const auto& section = positions[graph.SampleSection[k]];
                if (section.empty())
                {
                    continue;
                }
                Math::Vector3 position(Math::kZero);
                for (uint32_t c = 0; c < 3; ++c)
                {
                    position = position + section[graph.SampleVertices[k][c]] * graph.SampleWeights[k][c];
                }
                centers[graph.SampleCell[k]] = centers[graph.SampleCell[k]] + position;
                ++counts[graph.SampleCell[k]];
            }
            for (size_t cell = 0; cell < cellCount; ++cell)
            {
                if (counts[cell])
                {
                    centers[cell] = centers[cell] * (1.0f / counts[cell]);
                }
            }

            // 셀 좌표로 셀 id를 찾는 표를 만든다.
            const auto makeKey = [](int64_t x, int64_t y, int64_t z)
            {
                return uint64_t((x * 65536 + y) * 65536 + z);
            };
            std::unordered_map<uint64_t, uint32_t> lookup;
            for (uint32_t cell = 0; cell < cellCount; ++cell)
            {
                const auto& c = graph.CellCoords[cell];
                lookup.emplace(makeKey(c[0], c[1], c[2]), cell);
            }

            // 셀마다 십자를 그리고 양의 방향 이웃 중 같은 side인 셀과 간선을 잇는다.
            for (uint32_t cell = 0; cell < cellCount; ++cell)
            {
                if (!counts[cell])
                {
                    continue;
                }
                const Color color = TopologyIslandColor(labels.CellIsland[cell]);
                const Math::Vector3 center = centers[cell];
                lines.push_back({ center - Math::Vector3(crossHalf, 0, 0), center + Math::Vector3(crossHalf, 0, 0), color });
                lines.push_back({ center - Math::Vector3(0, crossHalf, 0), center + Math::Vector3(0, crossHalf, 0), color });
                lines.push_back({ center - Math::Vector3(0, 0, crossHalf), center + Math::Vector3(0, 0, crossHalf), color });
                const auto& c = graph.CellCoords[cell];
                for (int dx = -1; dx <= 1; ++dx)
                {
                    for (int dy = -1; dy <= 1; ++dy)
                    {
                        for (int dz = -1; dz <= 1; ++dz)
                        {
                            // 이웃 종류에 맞는 양의 방향 오프셋이 아니면 건너뛴다.
                            if (!IsForwardNeighborOffset(dx, dy, dz, neighborhood))
                            {
                                continue;
                            }

                            // 점유되고 side가 같은 이웃만 잇는다. side 0은 +1로 본다.
                            const int64_t nx = int64_t(c[0]) + dx;
                            const int64_t ny = int64_t(c[1]) + dy;
                            const int64_t nz = int64_t(c[2]) + dz;
                            if (nx < 0 || ny < 0 || nz < 0)
                            {
                                continue;
                            }
                            const auto found = lookup.find(makeKey(nx, ny, nz));
                            if (found == lookup.end() || !counts[found->second])
                            {
                                continue;
                            }
                            if ((cellSides[found->second] >= 0) != (cellSides[cell] >= 0))
                            {
                                continue;
                            }
                            lines.push_back({ center, centers[found->second], color });
                        }
                    }
                }
            }
        }

        // 셀 아일랜드에 속한 정점들의 bind weight를 palette 인덱스별로 합산한다.
        std::vector<std::map<uint32_t, double>> CellIslandJointScores(const std::vector<SkinCpuSection>& sections,
            const std::vector<std::vector<uint32_t>>& vertexIsland, uint32_t islandCount)
        {
            // 정점의 네 weight를 자기 아일랜드의 joint 점수에 더한다.
            std::vector<std::map<uint32_t, double>> scores(islandCount);
            for (size_t s = 0; s < sections.size(); ++s)
            {
                for (size_t v = 0; v < sections[s].size(); ++v)
                {
                    const auto& vertex = sections[s][v];
                    for (uint32_t j = 0; j < 4; ++j)
                    {
                        if (vertex.Weights[j])
                        {
                            scores[vertexIsland[s][v]][sections[s].StartJoint + vertex.Joints[j]] += vertex.Weights[j];
                        }
                    }
                }
            }
            return scores;
        }

        // 투표·선택·추출 패스가 공유하는 root signature와 compute PSO를 준비한다.
        void InitializePatch()
        {
            // 여러 C 요청 사이에 파이프라인을 재사용한다.
            static bool initialized = false;
            if (initialized) return;
            PatchRoot.Reset(13);
            PatchRoot[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 4; ++i) PatchRoot[i + 1].InitAsBufferSRV(i);
            for (uint32_t i = 0; i < 6; ++i) PatchRoot[i + 5].InitAsBufferUAV(i);
            PatchRoot[11].InitAsBufferSRV(4);
            PatchRoot[12].InitAsBufferUAV(6);
            PatchRoot.Finalize(L"Skeletal patch extraction");
            PatchPSO.SetRootSignature(PatchRoot);
            PatchPSO.SetComputeShader(g_pPatchExtractCS, sizeof(g_pPatchExtractCS));
            PatchPSO.Finalize();

            initialized = true;
        }

        // GPU 옵션이 켜진 첫 요청에서만 거리·UF·번호·투표 파이프라인을 준비한다.
        void InitializeDistance()
        {
            // 여러 요청에서 거리 파이프라인을 재사용한다.
            static bool initialized = false;
            if (initialized)
                return;

            // section 격자와 job 소유 코너 출력을 별도로 바인딩한다.
            DistanceRoot.Reset(9);
            DistanceRoot[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 6; ++i)
                DistanceRoot[i + 1].InitAsBufferSRV(i);
            for (uint32_t i = 0; i < 2; ++i)
                DistanceRoot[i + 7].InitAsBufferUAV(i);
            DistanceRoot.Finalize(L"Island distances");
            DistancePSO.SetRootSignature(DistanceRoot);
            DistancePSO.SetComputeShader(g_pIslandDistanceCS, sizeof(g_pIslandDistanceCS));
            DistancePSO.Finalize();

            // 거리 옵션과 같은 지연 초기화로 UF 파이프라인을 만든다.
            UnionRoot.Reset(9);
            UnionRoot[0].InitAsConstants(0, 4);
            for (uint32_t i = 0; i < 3; ++i)
                UnionRoot[i + 1].InitAsBufferSRV(i);
            for (uint32_t i = 0; i < 5; ++i)
                UnionRoot[i + 4].InitAsBufferUAV(i);
            UnionRoot.Finalize(L"Island union find");
            ComputePSO* psos[] = { &HookPSO, &JumpPSO, &VerifyPSO, &NumberPSO };
            const void* shaders[] = { g_pIslandHookCS, g_pIslandJumpCS, g_pIslandVerifyCS, g_pIslandNumberCS };
            const size_t sizes[] = { sizeof(g_pIslandHookCS), sizeof(g_pIslandJumpCS), sizeof(g_pIslandVerifyCS), sizeof(g_pIslandNumberCS) };
            for (uint32_t i = 0; i < 4; ++i)
            {
                // 각 UF·번호 셰이더를 공통 루트에 연결한다.
                psos[i]->SetRootSignature(UnionRoot);
                psos[i]->SetComputeShader(shaders[i], sizes[i]);
                psos[i]->Finalize();
            }
            // 투표 입력과 결과를 별도의 루트로 연결한다.
            VoteRoot.Reset(8);
            VoteRoot[0].InitAsConstants(0, 4);
            for (uint32_t i = 0; i < 5; ++i)
                VoteRoot[i + 1].InitAsBufferSRV(i);
            for (uint32_t i = 0; i < 2; ++i)
                VoteRoot[i + 6].InitAsBufferUAV(i);
            VoteRoot.Finalize(L"Island votes");
            VotePSO.SetRootSignature(VoteRoot);
            VotePSO.SetComputeShader(g_pIslandVoteCS, sizeof(g_pIslandVoteCS));
            VotePSO.Finalize();
            initialized = true;
        }
    }
    // 현재 joint 원점을 GPU pose와 같은 월드로 옮기고 section별 팔레트를 모은다.
    SegmentJointSelection SelectSegmentJoint(const ModelInstance& pose, const MeshComponent& component,
        const SceneTransform& owner, const MeshCutting::CutPlaneData& plane)
    {
        // 유효 pose의 위치만 읽으며 정점 readback은 하지 않는다.
        if (!pose.HasValidPose() || component.GetDeformBinding().Pose != PoseMode::FollowModel)
            return {};
        const auto& model = *pose.GetModel();
        std::vector<Math::Vector3> positions(model.m_NumNodes);

        // 이 컴포넌트가 참조하는 mesh의 skin joint만 선택 집합에 포함한다.
        std::vector<std::vector<uint32_t>> palettes;
        const auto& geometry = *component.GetGeometry();
        for (size_t section = 0; section < geometry.GetSections().size(); ++section)
        {
            const auto& mesh = geometry.GetMesh(section);
            palettes.emplace_back(model.m_JointIndices.get() + mesh.startJoint,
                model.m_JointIndices.get() + mesh.startJoint + mesh.numJoints);
            const auto jointToWorld = JointFrameToWorld(pose, component, owner, section);
            for (uint32_t node : palettes.back())
                positions[node] = Math::Vector3(jointToWorld
                    * Math::Vector4(Math::Vector3(pose.GetNodeTransforms()[node].GetW()), 1));
        }
        return SelectSegmentJoint(pose.GetPoseParents(), positions, palettes, plane, model.m_InfluencingJoints);
    }

    enum class RigSelectionReason { Selected, NoHits, NoCandidates, AlreadyBroken, D5 };
    struct RigSelection
    {
        ObjectId Owner = InvalidObjectId;
        SegmentJointSelection Selected;
        bool HasHits = false, AlreadyBroken = false;
        RigSelectionReason Reason = RigSelectionReason::NoHits;
        std::map<ObjectId, std::set<uint32_t>> OriginalHits, DynamicNodes;
        std::map<ObjectId, std::vector<RigSegment>> Unavailable;
        std::set<std::string> Rejections;
    };

    // 유한 평면 hit에서 선분을 선택하고 body·incoming·조회 실패 사유를 구분해 보존한다.
    static RigSelection SelectRigSegment(const Scene& scene, PhysicsSystem& physics,
        const MeshCutting::CutPlaneData& plane)
    {
        // 반두께 1 mm인 평면 상자로 등록 body를 조회한다.
        using namespace Math;
        RigSelection result;
        const Vector3 center = (plane.WorldCorners[0] + plane.WorldCorners[2]) * 0.5f;
        const Vector3 width = (plane.WorldCorners[1] - plane.WorldCorners[0]) * 0.5f;
        const Vector3 height = (plane.WorldCorners[3] - plane.WorldCorners[0]) * 0.5f;
        const Vector3 x = Normalize(width), y = Vector3(plane.WorldPlane), z = Normalize(Cross(x, y));
        DirectX::XMFLOAT4 q;
        DirectX::XMStoreFloat4(&q, Normalize(Quaternion(Matrix3(x, y, z))));
        std::vector<MiniPhysics::BodyHandle> hits;
        physics.CollideOrientedBox({ float(center.GetX()), float(center.GetY()), float(center.GetZ()) },
            { q.x, q.y, q.z, q.w }, { float(Length(width)) + std::abs(float(Dot(height, x))),
                0.001, std::abs(float(Dot(height, z))) }, hits);

        // owner별 Kinematic hit를 모으고 D5 후보는 body 중심 거리로 고른다.
        auto& skeletal = physics.GetSkeletalPhysics();
        std::map<ObjectId, std::set<uint32_t>> nodes;
        auto closest = std::make_tuple(FLT_MAX, InvalidObjectId, UINT32_MAX);
        for (auto hit : hits)
        {
            ObjectId owner;
            uint32_t body, node;
            if (!skeletal.FindBody(hit, owner, body, node)) continue;
            result.HasHits = true;
            result.OriginalHits[owner].insert(node);
            auto& hitNodes = nodes[owner];
            SkeletalRigState state;
            if (!skeletal.ReadDiagnosticState(owner, state) || body >= state.Bodies.size())
            {
                // 진단 상태 조회 실패는 끊긴 incoming과 별도 사유로 기록한다.
                result.Rejections.insert("pose_or_constraint_lookup_failed");
                continue;
            }

            // 원래 hit의 body 상태와 incoming 유무를 후보 판정과 별도로 보관한다.
            for (const auto& stateBody : state.Bodies)
                if (stateBody.Motion == MiniPhysics::BodyMotion::Dynamic)
                    result.DynamicNodes[owner].insert(stateBody.Node);
            const auto hitIncoming = skeletal.FindIncomingJoint(owner, node);
            if (hitIncoming.Status == IncomingJointStatus::AlreadyBroken)
                result.Rejections.insert("incoming_joint_broken");
            if (hitIncoming.Status == IncomingJointStatus::RigRoot)
                result.Rejections.insert("root_no_incoming");
            const auto& hitBody = state.Bodies[body];
            if (hitBody.Motion == MiniPhysics::BodyMotion::Dynamic)
            {
                // Dynamic hit는 선택 본으로 쓸 수 없지만 자식 끝점으로는 쓸 수 있다.
                result.Rejections.insert("selected_node_body_Dynamic");
                continue;
            }

            // Kinematic hit는 root를 포함해 기존 거리 기준으로 D5 후보에 보관한다.
            hitNodes.insert(node);
            const auto& c = hitBody.Frame.Position;
            closest = std::min(closest, std::make_tuple(float(LengthSquare(
                Vector3(float(c[0]), float(c[1]), float(c[2])) - center)), owner, node));
        }

        // 엄격한 부호 교차와 실제 평면 사각형 조건을 모든 선분에 함께 적용한다.
        const auto intersect = [&](const RigSegment& segment, Vector3& point)
        {
            // 월드 교점을 평면 로컬 사각형으로 옮겨 검사한다.
            const float a = Dot(plane.WorldPlane, Vector4(segment.ParentWorld, 1));
            const float b = Dot(plane.WorldPlane, Vector4(segment.ChildWorld, 1));
            if (!((a < 0 && b > 0) || (a > 0 && b < 0))) return false;
            point = segment.ParentWorld + (segment.ChildWorld - segment.ParentWorld) * (a / (a - b));
            const Vector3 local(plane.WorldToPlane * Vector4(point, 1));
            return std::abs(float(local.GetX())) <= plane.HalfWidth && std::abs(float(local.GetZ())) <= plane.HalfHeight;
        };
        auto best = std::make_tuple(FLT_MAX, InvalidObjectId, UINT32_MAX);
        for (const auto& entry : nodes)
        {
            std::vector<RigSegment> segments, unavailable;
            skeletal.BuildKinematicSegments(entry.first, segments, &unavailable);
            result.Unavailable[entry.first] = unavailable;
            for (const auto& segment : segments)
            {
                if (!entry.second.count(segment.ParentNode) && !entry.second.count(segment.ChildNode)) continue;
                Vector3 point;
                if (!intersect(segment, point)) continue;
                const auto candidate = std::make_tuple(float(LengthSquare(point - center)), entry.first, segment.ParentNode);
                if (candidate >= best) continue;
                best = candidate;
                result.Owner = entry.first;
                result.Selected.NodeId = segment.ParentNode;
                result.Selected.ChildNodeId = segment.ChildNode;
                result.Selected.Kind = SegmentJointKind::Segment;
                result.Selected.Intersection = point;
            }

            // 원래 hit와 관련된 탈락 선분에서 조회 실패와 실제 후보 제외 사유를 구분한다.
            for (const auto& segment : unavailable)
            {
                const auto& original = result.OriginalHits.at(entry.first);
                if (!original.count(segment.ParentNode) && !original.count(segment.ChildNode)) continue;
                if (segment.ConstraintLookupFailed)
                    result.Rejections.insert("constraint_lookup_failed");
                if (segment.PoseLookupFailed)
                {
                    // 복원되지 않은 원점으로는 교차 여부를 판단하지 않는다.
                    result.Rejections.insert("pose_lookup_failed");
                    continue;
                }

                // 교차하는 탈락 선분만 기존 AlreadyBroken 상태 판정에 반영한다.
                Vector3 point;
                if (!intersect(segment, point)) continue;
                result.AlreadyBroken = true;
                const auto dynamic = result.DynamicNodes.find(entry.first);
                if (dynamic != result.DynamicNodes.end() && dynamic->second.count(segment.ParentNode))
                    result.Rejections.insert("selected_node_body_Dynamic");
                const auto incoming = skeletal.FindIncomingJoint(entry.first, segment.ParentNode);
                if (incoming.Status == IncomingJointStatus::AlreadyBroken)
                    result.Rejections.insert("incoming_joint_broken");
                if (incoming.Status == IncomingJointStatus::RigRoot)
                    result.Rejections.insert("root_no_incoming");
            }
        }

        // 선택 결과에 실패 근거를 보존하여 호출자가 같은 조회 결과를 비교한다.
        result.Reason = result.Owner != InvalidObjectId ? RigSelectionReason::Selected
            : !result.HasHits ? RigSelectionReason::NoHits
            : result.AlreadyBroken ? RigSelectionReason::AlreadyBroken
            : std::get<1>(closest) != InvalidObjectId ? RigSelectionReason::D5 : RigSelectionReason::NoCandidates;

        // 선분이 없는 경우 같은 조회에서 고른 D5 후보 이름을 안전하게 남긴다.
        if (result.HasHits && result.Owner == InvalidObjectId && !result.AlreadyBroken)
        {
            const auto* object = scene.FindObject(std::get<1>(closest));
            WritePatchLogSafely([&]
            {
                // 이름 조회와 문자열 생성의 예외를 요청에서 격리한다.
                return "RigSelect no segment node=" + (object
                    ? SafeDiagnosticNodeName(object->GetName(), std::get<2>(closest)) : std::string("none")) + "\n";
            });
        }

        // D5 요청도 선택된 owner와 node를 추출 입력으로 전달한다.
        if (result.Reason == RigSelectionReason::D5)
        {
            result.Owner = std::get<1>(closest);
            result.Selected.NodeId = std::get<2>(closest);
        }

        // 선택된 root도 Request의 기존 RigRoot 거절 전에 진단 사유를 남긴다.
        if (result.Owner != InvalidObjectId &&
            skeletal.FindIncomingJoint(result.Owner, result.Selected.NodeId).Status == IncomingJointStatus::RigRoot)
            result.Rejections.insert("root_no_incoming");
        return result;
    }

    // Head 영역 안쪽 평면이 실제 Rig 조회에서 D5인지 확인한다.
    bool BuildRigBoneCutPlane(const Scene& scene, PhysicsSystem& physics, MeshCutting::CutPlaneData& plane, bool joint)
    {
        // Xbot DFS의 Head=8과 HeadTop_End=9로 현재 진단 중심을 만든다.
        using namespace Math;
        for (const auto& object : scene.GetObjects())
        {
            if (object->GetName() != "Xbot")
                continue;
            const auto& instance = *object->GetModelInstance();
            if (SafeDiagnosticNodeName("Xbot", 8) != "mixamorig:Head")
                return false;
            const auto frame = JointFrameToWorld(instance, *object->GetMeshComponents().front(), object->GetTransform(), 0);
            const Vector3 origin(frame * instance.GetNodeTransforms()[8].GetW());
            const Vector3 inward = Normalize(Vector3(frame * instance.GetNodeTransforms()[9].GetW()) - origin) * 0.06f;

            // Head 안쪽 3cm의 수평 평면과 원점을 지나는 수직 거절 평면을 만든다.
            const Vector3 normal = joint ? Vector3(kXUnitVector) : Vector3(kYUnitVector);
            const Vector3 x = Normalize(Cross(normal, joint ? Vector3(kYUnitVector) : Vector3(kXUnitVector)));
            if (!MeshCutting::TryBuildCutPlaneData({ 0.6f, 0.6f }, AffineTransform(Matrix3(x, normal, Cross(x, normal)),
                origin + inward - normal * float(Dot(normal, inward)) + normal * (joint ? 0.0f : 0.03f)), plane))
                return false;

            // 실제 조회가 Head의 D5일 때만 자동 C 요청에 사용한다.
            const auto selection = SelectRigSegment(scene, physics, plane);
            return selection.Reason == RigSelectionReason::D5 && selection.Owner == object->GetId() && selection.Selected.NodeId == 8;
        }
        return false;
    }

    // break 진단과 시연에서 같은 본 쌍의 현재 중점·수직 평면을 재사용한다.
    MeshCutting::CutPlaneData BuildSkeletalBreakPlane(const GameObject& object, uint32_t index)
    {
        // 전완·허벅지·팔의 기존 case 순서로 node 쌍을 골라 공통 평면 함수에 넘긴다.
        const uint32_t nodes[] = { 14, 60, 8 }, children[] = { 15, 61, 9 };
        return BuildSkeletalBonePlane(object, nodes[index], children[index]);
    }

    // 두 node 사이의 지정 비율 위치에서 본 방향을 법선으로 하는 절단 평면을 구성한다.
    MeshCutting::CutPlaneData BuildSkeletalBonePlane(const GameObject& object, uint32_t node, uint32_t child, float fraction)
    {
        // 두 node의 현재 월드 좌표를 조회한다.
        const auto& instance = *object.GetModelInstance();
        const auto frame = JointFrameToWorld(instance, *object.GetMeshComponents().front(), object.GetTransform(), 0);
        const Math::Vector3 a(frame * instance.GetNodeTransforms()[node].GetW());
        const Math::Vector3 b(frame * instance.GetNodeTransforms()[child].GetW());

        // 본 방향의 법선과 수직 축으로 평면 좌표계를 구성한다.
        const auto normal = Math::Normalize(b - a);
        const auto x = Math::Normalize(Math::Cross(normal, std::abs(float(normal.GetY())) < 0.9f
            ? Math::Vector3(Math::kYUnitVector) : Math::Vector3(Math::kXUnitVector)));

        // 부모 관절에서 자식 방향의 비율 위치에 유한 절단 평면을 생성하고 유효성을 검사한다.
        MeshCutting::CutPlaneData plane;
        if (!MeshCutting::TryBuildCutPlaneData({ 0.3f, 0.3f },
            Math::AffineTransform(Math::Matrix3(x, normal, Math::Cross(x, normal)), fraction == 0.5f ? (a + b) * 0.5f : a + (b - a) * fraction), plane))
            throw std::runtime_error("SP7b invalid diagnostic plane");
        return plane;
    }

    // 실제 C 경로의 Dynamic 집합을 검증하고 경계 측정의 PASS·FAIL·N/A를 요약한다.
    bool SkeletalPatchCopy::RunBreakChecks(Scene& scene, PhysicsSystem& physics, CuttingSystem& cutting)
    {
        // 첫 요청을 pause 상태로 제출해 결과가 실제 step까지 대기하는지 확인한다.
        if (m_RequestId && !physics.IsSimulationRunning())
        {
            BreakStatus status;
            if (physics.TryGetBreakResult(m_RequestId, status)) throw std::runtime_error("SP7b pause consumed request");
            PatchLog("SP7b pause waiting PASS\n");

            // 취소한 별도 요청은 이후 substep의 처리 수와 결과에 남지 않아야 한다.
            const auto cancelled = physics.QueueBreak(m_Break, true);
            physics.CancelBreak(cancelled);
            physics.SetSimulationRunning(true);
        }
        if (IsPending() || cutting.IsBusy()) return false;

        // 실패 결과 소비 뒤 원본 불변을 확인하고 같은 부위의 정상 요청을 허용한다.
        if (m_ForceStale)
        {
            VerifyStale(scene, physics);
            m_ForceStale = false;
            m_StaleStage = 1;
        }
        // 명시적 Rig 옵션만 진단 입력 모드를 바꾸며 기본 Parent 진단은 유지한다.
        std::wstring jointMode;
        CommandLineArgs::GetString(L"skeletal-joint-mode", jointMode);
        const auto checkMode = jointMode == L"rig" ? SkeletalJointMode::Rig : SkeletalJointMode::Parent;
        auto& skeletal = physics.GetSkeletalPhysics();
        const uint32_t nodes[] = { 14, 60, 8 }, joints[] = { 7, 43, 5 };

        // JointId는 독립 GLB palette 조사값을 쓰고 subtree는 pose ancestry에서 계산한다.
        if (m_CheckAwaiting)
        {
            SkeletalRigState state;
            const auto* object = scene.FindObject(m_Break.Owner);
            if (!object || !skeletal.ReadDiagnosticState(m_Break.Owner, state)) throw std::runtime_error("SP7b missing rig");
            const auto& parents = object->GetModelInstance()->GetPoseParents();
            const unsigned expectedJoint = joints[m_BreakCheck - 1];
            bool pass = m_Break.JointId == expectedJoint && m_LastBreakStatus == BreakStatus::Applied;
            PatchLog("SP7b case=%u expectedJoint=%u actualJoint=%u subtree expected/actual:\n", m_BreakCheck, expectedJoint, m_Break.JointId);
            for (const auto& body : state.Bodies)
            {
                bool expected = false;
                for (uint32_t p = body.Node; p != UINT32_MAX; p = parents[p])
                    expected |= p == nodes[m_BreakCheck - 1] || (m_BreakCheck == 2 && p == nodes[0]);
                const bool actual = body.Motion == MiniPhysics::BodyMotion::Dynamic;
                pass &= expected == actual;
                if (expected || actual) PatchLog("SP7b NodeId=%u expected=%u actual=%u\n", body.Node, expected, actual);
            }
            uint32_t root = state.Bodies.front().Node;
            pass &= skeletal.FindIncomingJoint(m_Break.Owner, root).Status == IncomingJointStatus::RigRoot;
            pass &= skeletal.FindIncomingJoint(m_Break.Owner, nodes[m_BreakCheck - 1]).Status == IncomingJointStatus::AlreadyBroken;
            PatchLog("SP7b subtree/root/AlreadyBroken %s\n", pass ? "PASS" : "FAIL");
            if (!pass) throw std::runtime_error("SP7b break diagnostic mismatch");

            // 실제 C 게시의 제거 범위와 owner 소유·충돌 제외를 함께 확인한다.
            if (!m_OffsetCopy)
            {
                const auto* source = object->FindMeshComponent(m_SourceMesh);
                const auto* mask = source ? source->GetTriangleMask() : nullptr;
                unsigned attached = 0;
                bool hidden = mask && mask->Pending == m_Removal && mask->AppliedRevision == mask->Revision;
                for (const auto& piece : object->GetMeshComponents())
                    if (piece->Attachment)
                    {
                        ++attached;
                        hidden &= !piece->IsSimulating() && piece->GetCollision().ConvexElems.empty();
                    }
                hidden &= attached == (m_BreakCheck == 2 ? 4u : 2u);
                PatchLog("SP7e case=%u hiddenRange=%s attached=%u noHullBody=%s\n", m_BreakCheck,
                    hidden ? "PASS" : "FAIL", attached, hidden ? "PASS" : "FAIL");
                if (!hidden) throw std::runtime_error("SP7e publish mismatch");
            }

            // 현재 segment에 수직인 평면으로 같은 C의 재절단 거절을 확인한다.
            Request(scene, BuildSkeletalBreakPlane(*object, m_BreakCheck - 1), false, checkMode, UINT32_MAX, &physics);
            if (IsPending() || m_Incoming.Status != IncomingJointStatus::AlreadyBroken) throw std::runtime_error("SP7b second C submitted");
            PatchLog("SP7b second C AlreadyBroken PASS\n");
            m_CheckAwaiting = false;
            m_StaleStage = 0;
        }
        if (m_BreakCheck == 3)
        {
            // 정상·Stale 요청만 처리되어 취소 요청이 관절을 건드리지 않았음을 확인한다.
            if (physics.GetQueuedBreakDiagnostics().Processed != (m_OffsetCopy ? 3u : 6u)) throw std::runtime_error("SP7e cancelled break processed");
            PatchLog("SP7e cancelled break PASS: processed=%u\n", physics.GetQueuedBreakDiagnostics().Processed);

            // 두 측정 시점의 결과를 합산하고 비교 대상이 전혀 없으면 N/A로 보고한다.
            const char* boundaryResult = m_BoundaryFailed ? "FAIL" : (m_BoundaryPassed ? "PASS" : "N/A");
            PatchLog("SP7e2 publish/after60 boundary <=1mm %s pass=%u fail=%u n/a=%u\n",
                boundaryResult, m_BoundaryPassed, m_BoundaryFailed, m_BoundaryNotApplicable);
            return true;
        }

        // 모델별 지정 segment 중점에서 실제 Parent 선택과 GPU A/B 절단을 요청한다.
        Reset(scene, physics, cutting);
        for (const auto& object : scene.GetObjects())
        {
            if (object->GetName() != (m_BreakCheck < 2 ? "Xbot" : "CesiumMan")) continue;
            const auto plane = BuildSkeletalBreakPlane(*object, m_BreakCheck);
            PhysicsSystem off(scene);
            Request(scene, plane, false, checkMode, UINT32_MAX, &off);
            if (IsPending()) throw std::runtime_error("SP7b physics off submitted cut");
            PatchLog("SP7b physics off PASS\n");
            if (!m_BreakCheck) physics.SetSimulationRunning(false);
            Request(scene, plane, true, checkMode, UINT32_MAX, &physics);
            if (!m_BreakReady || m_Incoming.Status != IncomingJointStatus::Ok) throw std::runtime_error("SP7b C rejected");
            // 각 부위의 첫 시도만 generation 불일치를 주입하고 게시 전 상태를 저장한다.
            if (!m_OffsetCopy && !m_StaleStage)
            {
                m_ForceStale = true;
                m_StaleBefore = ReadOriginalState(scene, physics);
                return false;
            }
            ++m_BreakCheck;
            m_CheckAwaiting = true;
            return false;
        }
        throw std::runtime_error("SP7b missing diagnostic model");
    }

    // Stale 전후 비교를 위해 원본 mask와 게시·물리 상태를 읽는다.
    std::vector<uint64_t> SkeletalPatchCopy::ReadOriginalState(const Scene& scene, PhysicsSystem& physics) const
    {
        // 준비 owner와 분리된 원본의 상태만 모은다.
        const auto* owner = scene.FindObject(m_Break.Owner);
        const auto* source = owner->FindMeshComponent(m_SourceMesh);
        const auto* mask = source->GetTriangleMask();
        std::vector<uint64_t> result{ physics.GetSkeletalPhysics().GetTopologyGeneration(m_Break.Owner),
            mask ? mask->Revision : 0, mask ? mask->Pending.size() : 0 };
        if (mask) result.insert(result.end(), mask->Pending.begin(), mask->Pending.end());
        for (const auto& mesh : owner->GetMeshComponents()) if (mesh->Attachment) result.push_back(mesh->GetId());
        result.push_back(UINT64_MAX);

        // 정상 애니메이션 이동과 분리하여 motion 종류와 끊긴 관절을 비교한다.
        SkeletalRigState rig;
        if (!physics.GetSkeletalPhysics().ReadDiagnosticState(m_Break.Owner, rig)) throw std::runtime_error("SP7e2 missing rig");
        for (const auto& body : rig.Bodies) result.push_back(unsigned(body.Motion));
        for (const auto& joint : rig.Joints) result.push_back(joint.Broken);
        return result;
    }

    // 실제 Stale 결과와 원본 mask·조각·물리 상태 보존을 검증한다.
    void SkeletalPatchCopy::VerifyStale(const Scene& scene, PhysicsSystem& physics)
    {
        // A/B 준비 owner까지 제거된 뒤 저장 상태와 대조한다.
        const bool pass = m_LastBreakStatus == BreakStatus::Stale && !m_CopyOwner
            && m_StaleBefore == ReadOriginalState(scene, physics);
        PatchLog("SP7e2 case=%u forcedStale=%s maskAttachmentsPhysicsUnchanged=%u noNewAB=%u\n",
            m_BreakCheck + 1, pass ? "PASS" : "FAIL", pass, !m_CopyOwner);
        if (!pass) throw std::runtime_error("SP7e2 Stale changed original");
    }

    // 현재 skinned PG 전체의 경계 포함 여부와 rim/cap 간격을 측정한다.
    static void MeasurePieceSkin(const MeshComponent* piece, uint32_t diagnosticCase, const char* side,
        const char* frame)
    {
        // 비활성 스킨이나 준비되지 않은 PG는 유효한 측정으로 인정하지 않는다.
        const bool skinned = piece && piece->HasSkinnedPatch();
        bool inBounds = false;
        double maximum = INFINITY;
        uint32_t pairCount = 0;
        double stumpMax = 0.0;
        Math::Matrix4 kFrame(Math::kIdentity);
        const bool haveFrame = piece && piece->TryGetRecutKFrame(kFrame);
        std::vector<Math::Vector3> posedNormals, posedPositions;
        if (skinned && piece->HasPoseBounds() && piece->GetPosedGeometry().IsReady())
        {
            // 실제 프레임의 RAW40을 동기 복사하고 VB 겸용 버퍼의 읽기 상태를 복원한다.
            const auto& skin = *piece->PatchSkin;
            const auto& posed = piece->GetPosedGeometry();
            auto& buffer = const_cast<ByteAddressBuffer&>(posed.GetBuffer());
            ReadbackBuffer readback;
            readback.Create(L"PS4 RAW40 readback", uint32_t(buffer.GetBufferSize()), 1);
            auto& context = GraphicsContext::Begin(L"PS4 piece bounds and seam");
            context.CopyBuffer(readback, buffer);
            context.TransitionResource(buffer, D3D12_RESOURCE_STATE_GENERIC_READ);
            context.Finish(true);

            // PG와 section 구는 모두 component-local이므로 추가 변환 없이 비교한다.
            const auto* bytes = static_cast<const uint8_t*>(readback.Map());
            std::vector<Math::Vector3> positions(skin.Vertices.Data.GetBufferSize() / skin.Vertices.Stride);
            inBounds = skin.VertexCount > 0;
            for (size_t i = 0; i < skin.Sections.size(); ++i)
            {
                const auto& section = skin.Sections[i];
                const auto bound = piece->GetLocalSectionBounds(i);
                const float radius = float(bound.GetRadius());
                for (uint32_t v = 0; v < section.VertexCount; ++v)
                {
                    float p[3];
                    std::memcpy(p, bytes + posed.GetOffset(piece->GetGeometry()->GetSections()[i].SourceMesh)
                        + size_t(v) * PosedGeometry::VertexStride, sizeof(p));
                    const Math::Vector3 position(p[0], p[1], p[2]);
                    positions.at(size_t(section.FirstVertex) + v) = position;

                    // AF-6 진단용으로 같은 정점의 준비된 법선(+12)도 모아 둔다.
                    float n[3];
                    std::memcpy(n, bytes + posed.GetOffset(piece->GetGeometry()->GetSections()[i].SourceMesh)
                        + size_t(v) * PosedGeometry::VertexStride + 12, sizeof(n));
                    if (posedNormals.size() != positions.size()) posedNormals.resize(positions.size());
                    posedNormals.at(size_t(section.FirstVertex) + v) = Math::Vector3(n[0], n[1], n[2]);
                    if (posedPositions.size() != positions.size()) posedPositions.resize(positions.size());
                    posedPositions.at(size_t(section.FirstVertex) + v) = position;
                    const float distance = float(Math::Length(position - bound.GetCenter()));
                    if (!std::isfinite(distance) || !std::isfinite(radius) || distance > radius + 1e-4f)
                        inBounds = false;
                }
            }
            readback.Unmap();

            // 누락 cap과 비유한 거리는 실패로 남기며, 각 split–cap 간격이 t_cut 대비 얼마나 변했는지 mm로 잰다.
            maximum = 0.0;
            pairCount = uint32_t(skin.RimCapPairs.size());
            for (const auto& pair : skin.RimCapPairs)
            {
                if (pair.Rim >= positions.size() || pair.Cap >= positions.size())
                {
                    maximum = INFINITY;
                    continue;
                }

                // AF-4: 옵션 1 rim이 t_cut 위치(= 옵션 0의 K 위치)에서 얼마나 움직였는지 기억한다.
                if (haveFrame)
                {
                    const Math::Vector3 optionZero(kFrame * Math::Vector4(pair.BindPosition[0], pair.BindPosition[1], pair.BindPosition[2], 1.0f));
                    stumpMax = std::max(stumpMax, double(float(Math::Length(positions[pair.Rim] - optionZero))) * 1000.0);
                }
                const float distance = float(Math::Length(positions[pair.Rim] - positions[pair.Cap]));
                const double change = std::abs(double(distance) - double(pair.DistanceAtCut)) * 1000.0;
                maximum = std::isfinite(change) ? std::max(maximum, change) : INFINITY;
            }
        }

        // AF-6: 옵션 1 cap 법선을 (a) 절단 시점 팔레트 항등 검사와 (b) 현재 팔레트 CPU 재계산 일치 검사로 확인한다.
        RecutCheckInput checkInput;
        if (skinned && haveFrame && PatchAnimFollowEnabled() && piece->PatchSkin->Snapshot.AnimFollow && !posedNormals.empty()
            && piece->TryGetRecutCheckInput(checkInput))
        {
            // 결과 skin Data를 동기 복사하고 읽기 상태를 복원한다.
            const auto& skin = *piece->PatchSkin;
            auto& data = const_cast<ByteAddressBuffer&>(skin.Vertices.Data);
            ReadbackBuffer dataReadback;
            dataReadback.Create(L"AF6 skin data readback", uint32_t(data.GetBufferSize()), 1);
            auto& copyContext = GraphicsContext::Begin(L"AF6 cap normal");
            copyContext.CopyBuffer(dataReadback, data);
            copyContext.TransitionResource(data, D3D12_RESOURCE_STATE_GENERIC_READ);
            copyContext.Finish(true);

            // 압축 법선 복원, 행렬의 법선 변환, 각도 계산 도구를 만든다.
            using namespace DirectX;
            const auto decode = [](uint32_t packed)
            {
                return XMVectorSet(float(packed & 1023) / 1023.0f * 2 - 1, float((packed >> 10) & 1023) / 1023.0f * 2 - 1,
                    float((packed >> 20) & 1023) / 1023.0f * 2 - 1, 0);
            };
            const auto normalOf = [](const Math::Matrix4& m)
            {
                return XMMatrixTranspose(XMMatrixInverse(nullptr, XMMATRIX(m)));
            };
            const auto degrees = [](XMVECTOR x, XMVECTOR y)
            {
                const float cosine = std::min(1.0f, std::max(-1.0f, XMVectorGetX(XMVector3Dot(XMVector3Normalize(x), XMVector3Normalize(y)))));
                return std::acos(cosine) * 57.29578f;
            };

            // 절단 시점과 실제 현재 법선을 사용한다. LB 위치 행렬은 역전치하지 않는다.
            std::vector<XMMATRIX> cutNormals, nowNormals;
            for (const auto& joint : skin.Snapshot.JointsAtCut)
                cutNormals.push_back(normalOf(joint.posXform));
            cutNormals.push_back(XMMatrixIdentity());
            for (const auto& matrix : checkInput.NormalPalette)
                nowNormals.push_back(Math::Matrix4(matrix));
            const XMMATRIX nodeNormal = normalOf(checkInput.Node);
            const bool paletteOk = nowNormals.size() == cutNormals.size();

            // 한 끝점의 지정한 방향(원본 법선 +12 또는 cap의 되돌린 법선 +16)을 주어진 팔레트의 법선 행렬로 스키닝해 정규화한다.
            const auto skinEndpoint = [&](const uint8_t* endpoint, const std::vector<XMMATRIX>& normals, size_t vectorOffset)
            {
                uint32_t words[4] = {};
                std::memcpy(words, endpoint + 20, sizeof(words));
                uint32_t packed = 0;
                std::memcpy(&packed, endpoint + vectorOffset, sizeof(packed));
                const XMVECTOR source = decode(packed);
                const uint32_t indices[4] = { words[0] & 65535, words[0] >> 16, words[1] & 65535, words[1] >> 16 };
                float weights[4] = { float(words[2] & 65535), float(words[2] >> 16), float(words[3] & 65535), float(words[3] >> 16) };
                const float sum = std::max(weights[0] + weights[1] + weights[2] + weights[3], 1.0f);
                XMVECTOR result = XMVectorZero();
                for (int k = 0; k < 4; ++k)
                    result += XMVector3TransformNormal(source, normals.at(indices[k])) * (weights[k] / sum);
                return XMVector3Normalize(result);
            };

            // cap 정점마다 (a) 절단 시점 팔레트 결과 대 K 법선, (b) 현재 팔레트 CPU 결과 대 준비된 법선, INFO 현재 K 법선 대비를 모은다.
            const auto* record = static_cast<const uint8_t*>(dataReadback.Map());
            float identityMax = 0.0f, matchMax = 0.0f, followMax = 0.0f;
            uint32_t count = 0;
            for (const auto& pair : skin.RimCapPairs)
            {
                if (!paletteOk || pair.Cap >= posedNormals.size()) continue;
                const uint8_t* vertex = record + size_t(pair.Cap) * skin.Vertices.Stride;
                const uint8_t* end = vertex + skin.Vertices.Stride;
                uint32_t packed = 0, cut0 = 0, cut1 = 0;
                float t = 0;
                std::memcpy(&packed, vertex + 12, sizeof(packed));
                std::memcpy(&t, end - 92, sizeof(t));
                std::memcpy(&cut0, end - 88 + 16, sizeof(cut0));
                std::memcpy(&cut1, end - 52 + 16, sizeof(cut1));
                if (!cut0 || !cut1) continue;
                const XMVECTOR bind = decode(packed);

                // (a) 팔레트 전체를 절단 시점 값으로 두면 되돌린 방향의 스키닝이 K 법선이 되어 결과가 K 법선(bind)과 같아야 한다.
                const XMVECTOR cutN = XMVector3Normalize(bind);
                const XMVECTOR b0c = skinEndpoint(end - 88, cutNormals, 16), b1c = skinEndpoint(end - 52, cutNormals, 16);
                const XMVECTOR identity = XMVectorLerp(b0c, b1c, t);
                identityMax = std::max(identityMax, degrees(identity, cutN));

                // (b) 현재 팔레트로 같은 식을 CPU에서 계산해 노드 변환 뒤 PreparePoseCS 출력과 비교한다.
                const XMVECTOR nowK = XMVector3TransformNormal(bind, nowNormals.back());
                const XMVECTOR b0 = skinEndpoint(end - 88, nowNormals, 16), b1 = skinEndpoint(end - 52, nowNormals, 16);
                const XMVECTOR expected = XMVector3TransformNormal(XMVectorLerp(b0, b1, t), nodeNormal);
                const XMVECTOR actual = XMVECTOR(posedNormals[pair.Cap]);
                matchMax = std::max(matchMax, degrees(expected, actual));

                // INFO: 절단 시점 이후 애니메이션 진행으로 현재 K 법선에서 벗어난 각도다.
                followMax = std::max(followMax, degrees(XMVector3TransformNormal(nowK, nodeNormal), actual));
                ++count;
            }

            // 독립 검사: cap 삼각형 면 법선(외적, 현재 K 법선과 부호 맞춤)과 각 cap 정점의 준비된 법선 사이 각도를 잰다.
            ReadbackBuffer geometryReadback;
            auto& geometryData = const_cast<ByteAddressBuffer&>(piece->GetGeometry()->GetGenerated()->Data);
            geometryReadback.Create(L"AF6 cap geometry readback", uint32_t(geometryData.GetBufferSize()), 1);
            auto& geometryContext = GraphicsContext::Begin(L"AF6 cap face normal");
            geometryContext.CopyBuffer(geometryReadback, geometryData);
            geometryContext.TransitionResource(geometryData, D3D12_RESOURCE_STATE_GENERIC_READ);
            geometryContext.Finish(true);
            const auto* geometryBytes = static_cast<const uint8_t*>(geometryReadback.Map());
            double faceSum = 0.0;
            float faceMax = 0.0f;
            uint32_t faceCount = 0;
            for (size_t m = 0; m < piece->GetGeometry()->GetSections().size() && paletteOk; ++m)
            {
                const Mesh& mesh = piece->GetGeometry()->GetMesh(m);
                if (mesh.materialCBV != 0xffff) continue;
                for (uint32_t i = 0; i + 2 < mesh.ibSize / 4; i += 3)
                {
                    uint32_t index[3];
                    std::memcpy(index, geometryBytes + mesh.ibOffset + i * 4, sizeof(index));
                    bool valid = true;
                    for (auto& value : index)
                    {
                        value += mesh.vbOffset / 28;
                        valid &= value < posedPositions.size();
                    }
                    if (!valid) continue;
                    XMVECTOR face = XMVector3Cross(XMVECTOR(posedPositions[index[1]]) - XMVECTOR(posedPositions[index[0]]),
                        XMVECTOR(posedPositions[index[2]]) - XMVECTOR(posedPositions[index[0]]));
                    if (XMVectorGetX(XMVector3LengthSq(face)) < 1e-16f) continue;
                    uint32_t packed = 0;
                    std::memcpy(&packed, record + size_t(index[0]) * skin.Vertices.Stride + 12, sizeof(packed));
                    const XMVECTOR reference = XMVector3TransformNormal(XMVector3TransformNormal(decode(packed), nowNormals.back()), nodeNormal);
                    if (XMVectorGetX(XMVector3Dot(face, reference)) < 0) face = -face;
                    for (uint32_t corner = 0; corner < 3; ++corner)
                    {
                        const float angle = degrees(face, XMVECTOR(posedNormals[index[corner]]));
                        faceSum += angle;
                        faceMax = std::max(faceMax, angle);
                        ++faceCount;
                    }
                }
            }
            geometryReadback.Unmap();
            dataReadback.Unmap();
            const float faceMean = faceCount ? float(faceSum / faceCount) : 0.0f;
            PatchLog("AF6 capnormal frame=%s side=%s n=%u identity_deg=%.4f match_deg=%.4f info_vsK_deg=%.4f faceMeanDeg=%.4f faceMaxDeg=%.4f faceN=%u result=%s\n", frame, side, count,
                identityMax, matchMax, followMax, faceMean, faceMax, faceCount,
                count && faceCount && matchMax <= 0.5f && faceMean <= 5.0f && (std::strcmp(frame, "publish") != 0 || identityMax <= 0.5f) ? "PASS" : "FAIL");
        }

        // 스키닝·전체 정점 경계·대응 쌍·0.01 mm 간격을 모두 만족해야 통과한다.
        const bool pass = skinned && inBounds && pairCount > 0 && maximum <= 0.01;
        PatchLog("PS4 case=%u side=%s frame=%s skinned=%d inBounds=%d rimCapPairs=%u rimCapChangeMax_mm=%.6f result=%s\n",
            diagnosticCase, side, frame, int(skinned), int(inBounds), pairCount, maximum, pass ? "PASS" : "FAIL");

        // AF-4: 옵션 1에서 끝점 보간 rim·cap 간격 변화가 용접 허용치(0.01 mm) 이하인지와 그루터기 이동량을 기록한다.
        if (skinned && PatchAnimFollowEnabled() && piece->PatchSkin->Snapshot.AnimFollow)
        {
            const auto& skin = *piece->PatchSkin;
            PatchLog("AF4 rimcap case=%u side=%s frame=%s gapChangeMax=%.6f weldTol=%.6f rims2=%u caps3=%u fallbackK=%u stumpDispMax_mm=%.6f result=%s\n",
                diagnosticCase, side, frame, maximum, 0.01, skin.Rims2, skin.Caps3, skin.FallbackK, stumpMax,
                maximum <= 0.01 && pairCount > 0 && skin.Rims2 + skin.Caps3 > 0 ? "PASS" : "FAIL");
        }
    }

    // 외곽 경계와 PS4를 측정하고 양쪽 고정 weight 및 두 시점의 결과를 기록한다.
    void SkeletalPatchCopy::MeasureBoundaryAt(const Scene& scene, const char* label)
    {
        // 양쪽 조각 id가 준비되지 않았으면 경계 진단을 수행하지 않는다.
        if (m_PieceIds.size() < 2)
        {
            return;
        }

        // 외곽 경계가 없는 case도 같은 타이밍 훅에서 A/B의 PS4를 측정한다.
        const auto* pieceOwner = scene.FindObject(m_Break.Owner);
        for (uint32_t side = 0; side < 2; ++side)
        {
            MeasurePieceSkin(pieceOwner ? pieceOwner->FindMeshComponent(m_PieceIds[side]) : nullptr,
                m_BreakCheck, side ? "B" : "A", label);
        }

        // 일치하는 유지 경계가 없으면 PASS·FAIL 집계에서 제외한다.
        if (m_Boundary.empty())
        {
            ++m_BoundaryNotApplicable;
            PatchLog("SP7e2 case=%u frame=%s result=%s boundary=%zu paired=%zu max_mm=%.6f frozenWeightMax=%g reason=%s\n",
                m_BreakCheck, label, "N/A", size_t(0), m_BoundaryPairs.size(),
                std::numeric_limits<float>::quiet_NaN(), 0.0, "noCoincidentKeptBoundary");
            return;
        }

        // C에서 대응시킨 정점 번호를 유지해 이후 프레임에도 같은 경계를 비교한다.
        const auto* owner = scene.FindObject(m_Break.Owner);
        const auto* source = owner ? owner->FindMeshComponent(m_SourceMesh) : nullptr;
        const MeshComponent* pieces[] = { owner ? owner->FindMeshComponent(m_PieceIds[0]) : nullptr,
            owner ? owner->FindMeshComponent(m_PieceIds[1]) : nullptr };
        float error = std::numeric_limits<float>::quiet_NaN();
        const bool measured = source && MeasureBoundary(*owner, *source, pieces, m_Boundary, m_BoundaryPairs, error);
        const float maximum = error * 1000;

        // 각 조각에 실제 대응된 경계만 사용해 정규화한 고정 joint weight 합의 최댓값을 구한다.
        float frozenWeightMax[2]{};
        for (const auto& pair : m_BoundaryPairs)
        {
            if (pair.Piece >= m_SkinSnapshots.size() || pair.Boundary >= m_BoundarySkins.size())
            {
                continue;
            }
            const auto& skin = m_BoundarySkins[pair.Boundary];
            const auto& frozen = m_SkinSnapshots[pair.Piece].Frozen;
            uint32_t total = 0;
            uint32_t fixed = 0;
            for (uint32_t slot = 0; slot < 4; ++slot)
            {
                total += skin.Weights[slot];
                if (skin.Joints[slot] < frozen.size() && frozen[skin.Joints[slot]])
                {
                    fixed += skin.Weights[slot];
                }
            }
            if (total)
            {
                frozenWeightMax[pair.Piece] = std::max(frozenWeightMax[pair.Piece], float(fixed) / total);
            }
        }

        // 보조 로그에서 A/B별 측정 경로와 고정 weight를 구별하고 case 로그에는 양쪽 최댓값을 남긴다.
        for (uint32_t side = 0; side < 2; ++side)
        {
            PatchLog("SP7e2 boundary side=%s case=%u frame=%s path=%s frozenWeightMax=%g\n",
                side ? "B" : "A", m_BreakCheck, label, !pieces[side] ? "missing"
                : (pieces[side]->HasSkinnedPatch() ? "skinnedPG_RAW40" : "rigidData_28B"), frozenWeightMax[side]);
        }

        // 두 시점 모두 1 mm 초과, 비정상 거리, 짝 누락 또는 측정 불가를 실패로 센다.
        const bool pass = measured && std::isfinite(maximum) && maximum <= 1;
        if (pass)
        {
            ++m_BoundaryPassed;
        }
        else
        {
            ++m_BoundaryFailed;
        }
        const char* reason = !measured ? "invalidDistanceOrMissingPairsOrReadbackUnavailable"
            : (!std::isfinite(maximum) ? "invalidDistance" : (pass ? "within1mm" : "over1mm"));
        PatchLog("SP7e2 case=%u frame=%s result=%s boundary=%zu paired=%zu max_mm=%.6f frozenWeightMax=%g reason=%s\n",
            m_BreakCheck, label, pass ? "PASS" : "FAIL", m_Boundary.size(), m_BoundaryPairs.size(), maximum,
            std::max(frozenWeightMax[0], frozenWeightMax[1]), reason);
    }

    // 현재 owner의 최종 pose로 자동 rig를 등록하고 생성 시간과 초기 겹침을 기록한다.
    bool RegisterAutoRig(const Scene& scene, ObjectId owner, SkeletalPhysicsSystem& skeletal)
    {
        // 등록된 owner는 scene 조회와 asset 생성 없이 성공으로 처리한다.
        if (skeletal.IsRegistered(owner)) return true;

        // 최초 생성부터 등록 완료까지의 시간만 측정한다.
        const int64_t start = SystemTime::GetCurrentTick();

        // snapshot을 구성할 owner와 모델 인스턴스를 확인한다.
        const auto* target = scene.FindObject(owner);
        if (!target || !target->GetModelInstance()) return false;

        // 미등록 owner만 현재 최종 pose와 section frame으로 asset을 만든다.
        const auto& instance = *target->GetModelInstance();
        const auto& model = *instance.GetModel();
        SkeletalPoseSnapshot pose;
        pose.Parents = instance.GetPoseParents();
        pose.Nodes.assign(instance.GetNodeTransforms(), instance.GetNodeTransforms() + model.m_NumNodes);
        for (const auto& component : target->GetMeshComponents())
            for (size_t s = 0; s < component->GetGeometry()->GetSections().size(); ++s)
                if (component->GetGeometry()->GetMesh(s).numJoints)
                    pose.Sections.push_back({ JointFrameToWorld(instance, *component, target->GetTransform(), s), {} });
        MiniPhysics::SkeletalPhysicsAsset asset;
        bool supportedMeshFrame = true;
        if (!BuildSkeletalPhysicsAsset(model, pose, asset))
        {
            PatchLog("RigPreload owner=%s failed: auto asset; SP6 only (reason on stderr)\n", target->GetName().c_str());
            return false;
        }

        // 물리 본에 종속된 mesh frame은 다음 writeback에서 지원하지 않는다.
        for (const auto& component : target->GetMeshComponents())
            for (size_t s = 0; s < component->GetGeometry()->GetSections().size(); ++s)
                if (component->GetGeometry()->GetMesh(s).numJoints)
                    for (uint32_t node = component->GetGeometry()->GetMesh(s).meshCBV;
                        node != UINT32_MAX; node = pose.Parents[node])
                        for (const auto& body : asset.Bodies) supportedMeshFrame &= body.Node != node;
        if (!supportedMeshFrame || pose.Sections.empty() || !skeletal.RegisterFromPose(owner, asset,
            pose.Parents, pose.Sections.front().E, pose.Nodes))
        {
            PatchLog("RigPreload owner=%s failed: unsupported mesh frame/pose or registration resource failure; SP6 only\n",
                target->GetName().c_str());
            return false;
        }

        // 등록 직후 실제 sensor 상태를 owner별로 기록한다.
        WriteRigPreloadLogSafely([&]
        {
            // 진단 문자열 할당도 예외 경계 안에서 수행한다.
            return "RigPreload sensors owner=" + target->GetName() + " sensor=" +
                std::to_string(skeletal.GetSensorCount(owner)) + "/" + std::to_string(asset.Bodies.size()) + "\n";
        });

        // 겹침 측정 실패도 이미 등록된 rig의 성공 상태를 바꾸지 않는다.
        const double milliseconds = SystemTime::TicksToMillisecs(SystemTime::GetCurrentTick() - start);
        CutPerf::Log("RIGPRELOAD owner=%s bodies=%zu joints=%zu create_register_ms=%.3f\n",
            target->GetName().c_str(), asset.Bodies.size(), asset.Joints.size(), milliseconds);
        try
        {
            LogRigPreload(target->GetName(), pose, asset, milliseconds);
        }
        catch (const std::exception& error)
        {
            Utility::Printf("RigPreload owner=%s bodies=%zu joints=%zu create_register_ms=%.6f measurement_failed=%s\n",
                target->GetName().c_str(), asset.Bodies.size(), asset.Joints.size(), milliseconds, error.what());
        }
        return true;
    }

    // 스켈레톤 루트를 포함한 몸통 그룹(boneCut·Frozen 포즈 제외)이고 애니메이션 입력이 있을 때만 AnimFollow와 O_k를 채운다.
    static void FillAnimFollow(PatchSkinSnapshot& snapshot, const MeshComponent& mesh,
        const std::vector<NodeId>& parents, const CutBodyGroups& groups, uint32_t body, bool boneCut)
    {
        // 애니메이션 입력이 없거나 boneCut·Frozen 포즈면 추종하지 않는다.
        snapshot.AnimFollow = false;
        snapshot.AnimOffset.clear();
        const auto* animNodes = mesh.GetPreparedAnimNodes();
        const auto* animJoints = mesh.GetPreparedAnimJoints();
        const auto* nodes = mesh.GetPreparedNodes();
        if (boneCut || snapshot.SourceFrozen || !animNodes || !animJoints || !nodes)
        {
            return;
        }

        // 부착 노드의 최상위 조상(스켈레톤 루트)이 몸통 그룹(0번)일 때만 몸통 쪽으로 본다.
        uint32_t top = snapshot.AttachNode;
        for (size_t depth = 0; top < parents.size() && parents[top] != UINT32_MAX && depth < parents.size(); ++depth)
        {
            top = parents[top];
        }
        if (body != 0 || top >= groups.NodeGroups.size() || groups.NodeGroups[top] != 0)
        {
            return;
        }

        // C(t_cut)를 구하고 고정 joint마다 O_k = inv(C·A_k)·JointsAtCut[k]를 저장한다.
        const Math::Matrix4 correction = nodes[snapshot.AttachNode] * Math::Invert(animNodes[snapshot.AttachNode]);
        snapshot.AnimOffset.assign(snapshot.JointsAtCut.size(), Math::Matrix4(Math::kIdentity));
        for (size_t k = 0; k < snapshot.JointsAtCut.size(); ++k)
        {
            if (!snapshot.Frozen[k])
            {
                continue;
            }
            snapshot.AnimOffset[k] = Math::Invert(correction * animJoints[k].posXform) * snapshot.JointsAtCut[k].posXform;
        }

        // C나 O_k에 비유한 값이 있으면 추종을 끄고 개수를 기록한다.
        auto finite = [](const Math::Matrix4& matrix)
        {
            DirectX::XMFLOAT4X4 value;
            DirectX::XMStoreFloat4x4(&value, matrix);
            for (uint32_t r = 0; r < 4; ++r)
            {
                for (uint32_t c = 0; c < 4; ++c)
                {
                    if (!std::isfinite(value.m[r][c]))
                    {
                        return false;
                    }
                }
            }
            return true;
        };
        uint32_t bad = finite(correction) ? 0u : 1u;
        for (size_t k = 0; k < snapshot.AnimOffset.size(); ++k)
        {
            bad += (snapshot.Frozen[k] && !finite(snapshot.AnimOffset[k])) ? 1u : 0u;
        }
        if (bad)
        {
            snapshot.AnimOffset.clear();
            PatchLog("AF2a nonfinite offsets=%u animFollow disabled\n", bad);
            return;
        }
        snapshot.AnimFollow = true;
    }

    // 두 팔레트의 고정 joint 위치·법선 행렬 차이의 절대 최댓값과 상대 최댓값(|기준 이동|과 1 중 큰 값으로 나눔)을 구한다.
    static void ComparePalettes(const PatchSkinSnapshot& snapshot, const std::vector<Joint>& actual,
        const std::vector<Joint>& expected, float& absolute, float& relative)
    {
        // 고정 joint마다 pos·nrm 행렬 성분 차이를 모은다.
        for (size_t k = 0; k < snapshot.JointsAtCut.size(); ++k)
        {
            if (!snapshot.Frozen[k])
            {
                continue;
            }
            const Math::Matrix4 a[] = { actual[k].posXform, Math::Matrix4(actual[k].nrmXform) };
            const Math::Matrix4 b[] = { expected[k].posXform, Math::Matrix4(expected[k].nrmXform) };
            const float scale = std::max(1.0f, float(Math::Length(Math::Vector3(expected[k].posXform.GetW()))));
            for (uint32_t m = 0; m < 2; ++m)
            {
                DirectX::XMFLOAT4X4 x, y;
                DirectX::XMStoreFloat4x4(&x, a[m]);
                DirectX::XMStoreFloat4x4(&y, b[m]);
                for (uint32_t r = 0; r < 4; ++r)
                {
                    for (uint32_t c = 0; c < 4; ++c)
                    {
                        const float error = std::abs(x.m[r][c] - y.m[r][c]);
                        absolute = !std::isfinite(error) ? INFINITY : std::max(absolute, error);
                        relative = !std::isfinite(error) ? INFINITY : std::max(relative, error / scale);
                    }
                }
            }
        }
    }

    // t_cut 입력으로 AnimFollow 식을 세 가지로 평가해 한 줄로 기록한다. 입력이 없으면 SKIP이다.
    static void CheckAnimFollowAtCut(const PatchSkinSnapshot& snapshot, const MeshComponent& mesh)
    {
        // 고정 joint 수를 센다.
        uint32_t frozen = 0;
        for (const uint8_t flag : snapshot.Frozen)
        {
            frozen += flag ? 1u : 0u;
        }

        // 추종이 꺼졌거나 애니메이션 입력이 없으면 평가하지 않고 SKIP으로 기록한다.
        const auto* animJoints = mesh.GetPreparedAnimJoints();
        const auto* animNodes = mesh.GetPreparedAnimNodes();
        const auto* nodes = mesh.GetPreparedNodes();
        if (!snapshot.AnimFollow || !animJoints || !animNodes || !nodes
            || snapshot.AnimOffset.size() != snapshot.JointsAtCut.size())
        {
            PatchLog("AF2a tcut palette frozen=%u animFollow=%d result=SKIP\n", frozen, int(snapshot.AnimFollow));
            return;
        }

        // 고정 강체 R(30도 회전+이동)과 S(-20도 회전+이동)를 만들고 입력 복사본을 준비한다.
        using namespace Math;
        const Matrix4 rigidR(Matrix3(Quaternion(Normalize(Vector3(1, 2, 3)), Scalar(0.5235988f))), Vector3(0.3f, -0.2f, 0.5f));
        const Matrix4 rigidS(Matrix3(Quaternion(Normalize(Vector3(-2, 1, 1)), Scalar(-0.3490659f))), Vector3(-0.1f, 0.4f, 0.2f));
        const size_t count = snapshot.JointsAtCut.size();
        const Joint* joints = snapshot.JointsAtCut.data();
        std::vector<Matrix4> movedNodes(nodes, nodes + snapshot.AttachNode + 1);
        std::vector<Joint> animCopy(animJoints, animJoints + count);
        std::vector<Joint> palette, expected(joints, joints + count);
        float absA0 = 0, relA0 = 0, absA = 0, relA = 0, absB = 0, relB = 0;

        // 검사 0: t_cut 입력 그대로면 팔레트가 JointsAtCut과 같아야 한다.
        BuildPatchPalette(snapshot, joints, movedNodes.data(), palette, animJoints, animNodes, true);
        ComparePalettes(snapshot, palette, expected, absA0, relA0);

        // 검사 a: 부착 노드를 R로 옮기면 팔레트가 R·JointsAtCut과 같아야 한다.
        movedNodes[snapshot.AttachNode] = rigidR * movedNodes[snapshot.AttachNode];
        BuildPatchPalette(snapshot, joints, movedNodes.data(), palette, animJoints, animNodes, true);
        for (size_t k = 0; k < count; ++k)
        {
            expected[k].posXform = rigidR * joints[k].posXform;
            expected[k].nrmXform = InverseTranspose(expected[k].posXform.Get3x3());
        }
        ComparePalettes(snapshot, palette, expected, absA, relA);

        // 검사 b: 애니메이션 joint를 S로 바꾸면 팔레트가 C0·S·inv(C0)·JointsAtCut과 같아야 한다.
        const Matrix4 c0 = nodes[snapshot.AttachNode] * Invert(animNodes[snapshot.AttachNode]);
        const Matrix4 conjugate = c0 * rigidS * Invert(c0);
        for (size_t k = 0; k < count; ++k)
        {
            animCopy[k].posXform = rigidS * animJoints[k].posXform;
            expected[k].posXform = conjugate * joints[k].posXform;
            expected[k].nrmXform = InverseTranspose(expected[k].posXform.Get3x3());
        }
        BuildPatchPalette(snapshot, joints, nodes, palette, animCopy.data(), animNodes, true);
        ComparePalettes(snapshot, palette, expected, absB, relB);

        // 세 결과를 한 줄로 기록한다(상대 오차 1e-5 이하면 PASS).
        const float worst = std::max({ relA0, relA, relB });
        PatchLog("AF2a tcut palette frozen=%u animFollow=1 identity abs=%g rel=%g | rigidR abs=%g rel=%g | animS abs=%g rel=%g result=%s\n",
            frozen, absA0, relA0, absA, relA, absB, relB, worst <= 1e-5f ? "PASS" : "FAIL");
    }

    // 형제 몸체의 Frozen 본이 각 부착 노드를 따르고 나머지는 현재 팔레트를 쓰는지 검사한다.
    static void CheckBodyPalettes(const MeshComponent& source, const std::vector<NodeId>& parents,
        const std::vector<uint32_t>& roots)
    {
        // 실제 모델 팔레트를 복사하고 서로 다른 본 이동을 가한 후속 포즈를 만든다.
        using namespace Math;
        const auto& model = *source.GetGeometry()->GetSource();
        const std::vector<uint32_t> jointNodes(model.m_JointIndices.get(), model.m_JointIndices.get() + model.m_NumJoints);
        const auto groups = BuildCutBodyGroups(parents, jointNodes, roots);
        std::vector<Matrix4> nodes(source.GetPreparedNodes(), source.GetPreparedNodes() + parents.size());
        std::vector<Joint> joints(source.GetPreparedJoints(), source.GetPreparedJoints() + model.m_NumJoints);
        for (size_t node = 0; node < nodes.size(); ++node)
            nodes[node] = Matrix4(Matrix3(kIdentity), Vector3(float(node + 1), 0, 0)) * nodes[node];
        for (auto& joint : joints)
            joint.posXform = Matrix4(Matrix3(kIdentity), Vector3(0, 7, 0)) * joint.posXform;

        // 각 몸체의 실제 팔레트 생성기를 호출해 고정 및 추종 슬롯의 위치를 비교한다.
        for (size_t body = 0; body < groups.AttachNodes.size(); ++body)
        {
            PatchSkinSnapshot snapshot;
            snapshot.AttachNode = groups.AttachNodes[body];
            snapshot.AttachAtCut = source.GetPreparedNodes()[snapshot.AttachNode];
            snapshot.JointsAtCut.assign(source.GetPreparedJoints(), source.GetPreparedJoints() + model.m_NumJoints);
            snapshot.Frozen = groups.FrozenPalettes[body];
            std::vector<Joint> palette;
            BuildPatchPalette(snapshot, joints.data(), nodes.data(), palette);
            const Matrix4 delta(Matrix3(kIdentity), Vector3(float(snapshot.AttachNode + 1), 0, 0));
            float maxError = 0;
            uint32_t frozen = 0;
            for (size_t k = 0; k < joints.size(); ++k)
            {
                // 0점과 비영점에서 고정 슬롯의 부착 이동 및 자기 그룹의 후속 포즈를 검사한다.
                const bool fixed = groups.NodeGroups[jointNodes[k]] != body;
                frozen += fixed;
                const Matrix4 expected = fixed ? delta * snapshot.JointsAtCut[k].posXform : joints[k].posXform;
                for (const Vector4 point : { Vector4(0, 0, 0, 1), Vector4(1, 2, 3, 1) })
                    maxError = std::max(maxError, float(Length(Vector3((palette[k].posXform * point) - (expected * point)))));
            }
            PatchLog("MI4c body=%zu attach=%u frozen=%u joints=%u paletteMaxError=%g result=%s\n",
                body, snapshot.AttachNode, frozen, model.m_NumJoints, maxError, maxError <= 1e-4f ? "PASS" : "FAIL");
            if (maxError > 1e-4f)
                throw std::runtime_error("MI4c Frozen palette mismatch");
        }
    }

    // 합집합 진단이 고른 형제 P_i와 그 선분 snapshot을 component-local 평면과 함께 담는다.
    struct UnionCutRequest
    {
        MeshCutting::CutPlaneData Plane;
        std::vector<uint32_t> Roots, Children, ChildSides;
    };

    // 형제 분기의 합집합과 자손이 있는 단일 본의 기존 마스크 경로를 GPU 출처 목록으로 대조한다.
    // 검증한 형제 쌍을 실제 다중 P 절단 진단 요청으로 반환한다.
    static UnionCutRequest CheckJointUnion(const MeshComponent& source, const Math::Matrix4& world,
        const std::vector<NodeId>& parents, const std::string& modelName)
    {
        // 제출 pose의 component-local 본 위치와 section 팔레트를 모은다.
        using namespace Math;
        const auto& input = *source.GetGeometry();
        const auto& model = *input.GetSource();
        std::vector<Vector3> positions(parents.size(), Vector3(kZero));
        uint32_t skinSection = 0;
        while (skinSection < input.GetSections().size() && !input.GetMesh(skinSection).numJoints)
            ++skinSection;
        if (skinSection == input.GetSections().size())
            throw std::runtime_error("MI4b no skin section");
        const Matrix4 frame = Invert(Matrix4(source.GetDeformBinding().SourceFrame.ToAffine()))
            * source.GetPreparedNodes()[input.GetMesh(skinSection).meshCBV];
        for (uint32_t node = 0; node < parents.size(); ++node)
            positions[node] = Vector3(frame * source.GetPreparedNodes()[node] * Vector4(0, 0, 0, 1));
        std::vector<std::vector<uint32_t>> palettes;
        for (uint32_t section = 0; section < input.GetSections().size(); ++section)
        {
            const auto& mesh = input.GetMesh(section);
            palettes.emplace_back(model.m_JointIndices.get() + mesh.startJoint,
                model.m_JointIndices.get() + mesh.startJoint + mesh.numJoints);
        }

        // 각 선분 중점 높이의 무한 평면에서 MI-4a가 고른 형제 P 둘을 찾는다.
        for (uint32_t child = 0; child < parents.size(); ++child)
        {
            if (parents[child] == UINT32_MAX)
                continue;
            MeshCutting::CutPlaneData plane;
            const Vector3 center = (positions[child] + positions[parents[child]]) * 0.5f;
            if (!MeshCutting::TryBuildCutPlaneData({100, 100},
                AffineTransform(Matrix3(kIdentity), center), plane))
                continue;
            const auto crossing = SelectCrossingJoints(parents, positions, palettes, plane, model.m_InfluencingJoints);
            for (size_t a = 0; a < crossing.size(); ++a)
                for (size_t b = a + 1; b < crossing.size(); ++b)
                {
                    const uint32_t first = crossing[a].NodeId, second = crossing[b].NodeId;
                    if (parents[first] == UINT32_MAX || parents[first] != parents[second])
                        continue;

                    // MI-4c는 형제의 세 몸체 팔레트만 검사하고 다중 절단이나 break를 제출하지 않는다.
                    CheckBodyPalettes(source, parents, {first, second});

                    // 두 P_i 본의 마스크를 한 작업으로 합치고 단일 P_i 및 중복 P도 검사한다.
                    PatchLog("MI4b siblings P=%u name=%s P=%u name=%s parent=%u planeY=%.6f\n",
                        first, SafeDiagnosticNodeName(modelName, first).c_str(), second,
                        SafeDiagnosticNodeName(modelName, second).c_str(), parents[first], float(center.GetY()));
                    const std::vector<std::vector<uint32_t>> cases = {{first}, {second}, {first, second}, {first, second, first}};
                    std::set<uint32_t> separate;
                    for (const auto& roots : cases)
                    {
                        // 합집합 경로에 같은 평면과 본 집합을 GPU 작업으로 제출한다.
                        SkeletalPatchJob job;
                        if (!job.Submit(source, plane, {}, world, first, true, false, parents, UINT32_MAX, roots))
                            throw std::runtime_error("MI4b union diagnostic submit failed");

                        // GPU 완료를 기다리고 출처 목록을 포함한 결과를 가져온다.
                        Graphics::g_CommandManager.IdleGPU();
                        SkeletalPatchResult result;
                        std::shared_ptr<SkeletalPatchBuffers> patch;
                        if (!job.Poll(result, patch))
                            throw std::runtime_error("MI4b union diagnostic poll failed");

                        // 개별 분기의 집합과 합집합을 비교하고 GPU·CPU 대조 결과를 기록한다.
                        const std::set<uint32_t> actual(patch->Triangles.begin(), patch->Triangles.end());
                        if (roots.size() == 1)
                            separate.insert(actual.begin(), actual.end());
                        else if (actual != separate || actual.empty())
                            throw std::runtime_error("MI4b union differs from separate P sets");
                        PatchLog("MI4b union roots=%zu P=%u,%u parent=%u tris=%u GPU==CPU PASS duplicates=0\n",
                            roots.size(), first, second, parents[first], result.PatchTriangles);
                    }

                    // 자손이 있는 본에서 기존 단일 선택과 같은 팔레트 마스크를 만든다.
                    if (std::find(parents.begin(), parents.end(), first) == parents.end())
                        throw std::runtime_error("MI4b n=1 diagnostic needs descendants");
                    std::vector<std::vector<uint32_t>> legacyMasks;
                    for (const auto& palette : palettes)
                    {
                        std::vector<uint32_t> mask;
                        for (uint32_t node : palette)
                            mask.push_back(node == first ? 1u : 0u);
                        legacyMasks.push_back(std::move(mask));
                    }

                    // 같은 입력을 기존 마스크 경로와 단일 루트 합집합 경로로 각각 GPU에 제출한다.
                    SkeletalPatchJob legacyJob, unionJob;
                    if (!legacyJob.Submit(source, plane, legacyMasks, world, first, true, false, parents)
                        || !unionJob.Submit(source, plane, {}, world, first, true, false, parents, UINT32_MAX, {first}))
                        throw std::runtime_error("MI4b n=1 diagnostic submit failed");

                    // 두 GPU 작업이 완료되면 각각의 출처 목록을 가져온다.
                    Graphics::g_CommandManager.IdleGPU();
                    SkeletalPatchResult legacyResult, unionResult;
                    std::shared_ptr<SkeletalPatchBuffers> legacyPatch, unionPatch;
                    if (!legacyJob.Poll(legacyResult, legacyPatch) || !unionJob.Poll(unionResult, unionPatch))
                        throw std::runtime_error("MI4b n=1 diagnostic poll failed");

                    // 출처 집합과 순서를 함께 비교하고 단일 본의 동등성 결과를 기록한다.
                    const std::set<uint32_t> legacySources(legacyPatch->Triangles.begin(), legacyPatch->Triangles.end());
                    const std::set<uint32_t> unionSources(unionPatch->Triangles.begin(), unionPatch->Triangles.end());
                    const bool equivalent = legacySources == unionSources && legacyPatch->Triangles == unionPatch->Triangles;
                    PatchLog("MI4b n=1 P=%u descendants=1 legacy=%u union=%u sourcesEquivalent=%s\n",
                        first, legacyResult.PatchTriangles, unionResult.PatchTriangles, equivalent ? "PASS" : "FAIL");
                    if (!equivalent)
                        throw std::runtime_error("MI4b n=1 legacy and union sources differ");

                    // 검증한 형제 쌍과 선택 자식 snapshot을 실제 다중 P 절단 진단에 넘긴다.
                    return { plane, { first, second }, { crossing[a].ChildNodeId, crossing[b].ChildNodeId },
                        { crossing[a].ChildSide, crossing[b].ChildSide } };
                }
        }

        // 교차하는 형제 쌍을 찾지 못하면 진단 실패를 기록하고 보고한다.
        PatchLog("MI4b FAIL no crossing sibling pair\n");
        throw std::runtime_error("MI4b no crossing sibling pair");
    }

    // 요청 평면에서 교차하는 최상위 P와 자식 방향을 component-local 공간으로 선택한다.
    static UnionCutRequest SelectMultiCut(const MeshComponent& source, const Math::Matrix4& world,
        const std::vector<NodeId>& parents, const MeshCutting::CutPlaneData& plane)
    {
        // 공통 skin frame으로 모든 본 위치와 section 팔레트를 모은다.
        using namespace Math;
        const auto& input = *source.GetGeometry();
        const auto& model = *input.GetSource();
        const auto* nodes = source.GetPreparedNodes();
        if (!nodes)
        {
            return {};
        }
        size_t firstSkin = 0;
        while (firstSkin < input.GetSections().size() && !input.GetMesh(firstSkin).numJoints)
        {
            ++firstSkin;
        }
        if (firstSkin == input.GetSections().size())
        {
            return {};
        }
        const Matrix4 frame = Invert(Matrix4(source.GetDeformBinding().SourceFrame.ToAffine()))
            * nodes[input.GetMesh(firstSkin).meshCBV];
        std::vector<Vector3> positions(parents.size(), Vector3(kZero));
        for (uint32_t node = 0; node < parents.size(); ++node)
        {
            positions[node] = Vector3(frame * nodes[node] * Vector4(0, 0, 0, 1));
        }
        std::vector<std::vector<uint32_t>> palettes;
        for (size_t section = 0; section < input.GetSections().size(); ++section)
        {
            const auto& mesh = input.GetMesh(section);
            palettes.emplace_back(model.m_JointIndices.get() + mesh.startJoint,
                model.m_JointIndices.get() + mesh.startJoint + mesh.numJoints);
        }

        // 무한 평면의 최상위 교차 집합과 자식 snapshot을 고정한다.
        UnionCutRequest result;
        result.Plane = plane;
        result.Plane.WorldPlane = Transpose(world) * plane.WorldPlane;
        result.Plane.WorldToPlane = plane.WorldToPlane * world;
        const Matrix4 inverseWorld = Invert(world);
        for (auto& corner : result.Plane.WorldCorners)
        {
            corner = Vector3(inverseWorld * Vector4(corner, 1));
        }
        for (const auto& crossing : SelectCrossingJoints(parents, positions, palettes,
            result.Plane, model.m_InfluencingJoints))
        {
            result.Roots.push_back(crossing.NodeId);
            result.Children.push_back(crossing.ChildNodeId);
            result.ChildSides.push_back(crossing.ChildSide);
        }
        return result;
    }

    // 평면과 선택 본을 고정하고 비동기 패치 추출을 요청한다.
    void SkeletalPatchCopy::Request(const Scene& scene, const MeshCutting::CutPlaneData& requestedPlane, bool diagnostic,
        SkeletalJointMode mode, uint32_t diagnosticCase, PhysicsSystem* physics)
    {
        // 절단 성능 로그의 요청 시작 시각을 기록한다.
        CutPerf::BeginRequest();

        // 명시 다중 옵션의 진단 평면만 요청 내부에서 교체한다.
        auto plane = requestedPlane;

        // 제거 상태와 패치 id가 없는 unmask 게시 이후에는 모든 후속 C를 거절한다.
        if (IsUnmaskDemoPublished())
        {
            PatchLog("Skeletal demo C rejected: unmask demo already published; restart to cut again\n");
            return;
        }

        // 새 C의 모드를 기록하고 대상 유무와 관계없이 이전 결과를 무효화한다.
        if (m_RequestId)
        {
            PatchLog("SP7b C waiting RequestId=%llu\n", m_RequestId);
            return;
        }
        m_MultiBreaks.clear();
        m_MultiPieces.clear();
        m_MultiPrepare = false;
        m_Mode = mode;
        m_DiagnosticCase = diagnosticCase;
        PatchLog("Skeletal patch C mode=%s\n", mode == SkeletalJointMode::Rig ? "Rig" : mode == SkeletalJointMode::Parent ? "Parent" : "Influence");
        m_Job.Cancel();
        m_ReadyCut.reset();
        DiscardBodyCompaction();
        m_Name.clear();
        m_Physics = physics;
        m_SkinPatch.reset();
        m_SkinSnapshots.clear();
        m_PendingDetach = {};
        m_RemainLengthTables.clear();
        m_PatchBoundaryFlags.clear();
        m_Attachments.clear();
        m_PieceIds.clear();
        m_BoundaryWait = 0;
        m_SkinPaletteErrors.clear();
        m_BodySides = { 0, 1 };
        m_SkinCheckCase = 0;
        m_SkinSectionNodes.clear();
        m_BreakReady = false;
        m_BonePending = false;
        m_BonePublication = {};

        // 기존 offset 복사본은 명시적 진단 옵션에서만 허용한다.
        uint32_t offsetCopy = 0;
        CommandLineArgs::GetInteger(L"skeletal-patch-offset-copy", offsetCopy);
        m_OffsetCopy = offsetCopy != 0;

        // 시연은 정상 C 경로를 사용하고 unmask 비교는 원본 제거 상태 게시를 생략한다.
        uint32_t unmask = 0;
        CommandLineArgs::GetInteger(L"skeletal-break-demo", m_Demo);
        uint32_t attachedRecut = 0;
        if (CommandLineArgs::GetInteger(L"attached-recut-checks", attachedRecut) && attachedRecut) m_Demo = 1;
        CommandLineArgs::GetInteger(L"skeletal-break-demo-unmask", unmask);
        m_DemoUnmask = m_Demo && unmask != 0;
        if (m_Demo) m_OffsetCopy = false;

        // 평면 로컬에서 가장 가까운 사각형 점을 월드로 돌려 경계 구와 비교한다.
        using namespace Math;
        const Vector3 center = (plane.WorldCorners[0] + plane.WorldCorners[2]) * 0.5f;
        const Matrix4 planeToWorld = Invert(plane.WorldToPlane);
        const GameObject* target = nullptr;
        const MeshComponent* mesh = nullptr;
        RigSelection rig;
        const bool rigQuery = mode == SkeletalJointMode::Rig && physics && physics->IsSimulationEnabled();
        if (mode != SkeletalJointMode::Influence && physics && physics->IsSimulationEnabled())
            rig = SelectRigSegment(scene, *physics, plane);
        float nearest = FLT_MAX;
        float legacyNearest = FLT_MAX;
        const GameObject* legacyTarget = nullptr;
        const MeshComponent* legacyMesh = nullptr;
        for (const auto& object : scene.GetObjects())
            for (const auto& component : object->GetMeshComponents())
            {
                // 비교 대상은 등록 여부와 관계없이 기존 경계 구로 검사한다.
                bool skin = false;
                for (size_t section = 0; section < component->GetGeometry()->GetSections().size(); ++section)
                    skin |= component->GetGeometry()->GetMesh(section).numJoints != 0;
                if (!object->GetModelInstance() || !component->HasPoseBounds() || !skin)
                    continue;
                const auto bounds = component->GetBoundingSphere(object->GetTransform());
                const Vector3 local = Vector3(plane.WorldToPlane * Vector4(bounds.GetCenter(), 1));
                const Vector3 closest = Vector3(planeToWorld * Vector4(
                    Clamp(float(local.GetX()), -plane.HalfWidth, plane.HalfWidth), 0,
                    Clamp(float(local.GetZ()), -plane.HalfHeight, plane.HalfHeight), 1));
                const float distance = LengthSquare(bounds.GetCenter() - center);
                if (float(LengthSquare(bounds.GetCenter() - closest)) > float(bounds.GetRadius() * bounds.GetRadius()))
                    continue;
                if (distance < legacyNearest)
                {
                    legacyNearest = distance;
                    legacyTarget = object.get();
                    legacyMesh = component.get();
                }

                // 실제 D6 요청은 미등록 owner만 기존 경계 구 후보로 유지한다.
                if (rigQuery && !rig.HasHits && physics->GetSkeletalPhysics().IsRegistered(object->GetId())) continue;
                if (distance >= nearest) continue;
                nearest = distance;
                target = object.get();
                mesh = component.get();
            }

        // 기존 선택을 한 번 계산하고 비교와 실제 요청에서 재사용한다.
        const bool influence = mode == SkeletalJointMode::Influence;
        auto legacy = influence || !legacyTarget ? SegmentJointSelection{}
            : SelectSegmentJoint(*legacyTarget->GetModelInstance(), *legacyMesh, legacyTarget->GetTransform(), plane);
        if (!influence && physics && physics->IsSimulationEnabled())
            WritePatchLogSafely([&]
            {
                // 성공과 실패 모두 owner와 노드 쌍 및 incoming 관절을 한 줄로 기록한다.
                auto& skeletal = physics->GetSkeletalPhysics();
                const auto* rigOwner = scene.FindObject(rig.Owner);
                const auto rigJoint = skeletal.FindIncomingJoint(rig.Owner, rig.Selected.NodeId);
                const auto legacyJoint = skeletal.FindIncomingJoint(legacyTarget ? legacyTarget->GetId() : InvalidObjectId, legacy.NodeId);
                const char* reasons[] = { "selected", "no_hits", "no_candidates", "AlreadyBroken", "D5" };
                std::ostringstream line;
                line << "RigSelect owner=" << (rigOwner ? rigOwner->GetName() : "none")
                    << " parent=" << rig.Selected.NodeId << " child=" << rig.Selected.ChildNodeId << " joint=" << rigJoint.JointId
                    << " result=" << reasons[unsigned(rig.Reason)]
                    << " legacy=" << (legacyTarget ? legacyTarget->GetName() : "none") << ':' << legacy.NodeId << '/' << legacy.ChildNodeId
                    << " joint=" << legacyJoint.JointId;
                const bool same = rigOwner && legacyTarget && rig.Owner == legacyTarget->GetId()
                    && legacy.Kind == SegmentJointKind::Segment && rig.Selected.NodeId == legacy.NodeId
                    && rig.Selected.ChildNodeId == legacy.ChildNodeId && rigJoint.JointId == legacyJoint.JointId;
                line << " compare=" << (same ? "match" : "mismatch");

                // 기존 상태 코드와 비교 필드 뒤에 후보 탈락의 구체적인 근거를 덧붙인다.
                for (const auto& rejection : rig.Rejections) line << " rejected=" << rejection;

                // 불일치는 실제 hit와 body 상태 및 선택 거리로 설명한다.
                if (!same)
                {
                    if (!rigOwner) line << " reason=rig_selection_none:" << reasons[unsigned(rig.Reason)];
                    if (!legacyTarget || legacy.NodeId == UINT32_MAX) line << " reason=legacy_selection_none";
                    else if (legacy.Kind != SegmentJointKind::Segment) line << " reason=legacy_leaf";
                    else
                    {
                        const auto owner = legacyTarget->GetId();
                        const auto hit = rig.OriginalHits.find(owner);
                        if (hit == rig.OriginalHits.end() || (!hit->second.count(legacy.NodeId) && !hit->second.count(legacy.ChildNodeId)))
                            line << " reason=legacy_segment_outside_hit";
                        const auto dynamic = rig.DynamicNodes.find(owner);
                        if (dynamic != rig.DynamicNodes.end())
                        {
                            // 선택 본의 Dynamic은 탈락 사유이며 자식 Dynamic은 애니메이션 끝점 사용 정보다.
                            if (dynamic->second.count(legacy.NodeId)) line << " reason=legacy_selected_node_body_Dynamic";
                            if (dynamic->second.count(legacy.ChildNodeId)) line << " info=legacy_child_Dynamic_animation_endpoint";
                        }
                        const auto unavailable = rig.Unavailable.find(owner);
                        if (unavailable != rig.Unavailable.end())
                            for (const auto& segment : unavailable->second)
                                if (segment.ParentNode == legacy.NodeId && segment.ChildNode == legacy.ChildNodeId)
                                    line << " reason=legacy_segment_unavailable";
                        if (rigOwner)
                        {
                            const float rigDistance = LengthSquare(rig.Selected.Intersection - center);
                            const float legacyDistance = LengthSquare(legacy.Intersection - center);
                            line << " reason=" << (rigDistance == legacyDistance ? "distance_tie" : "distance_difference")
                                << " rigDistance2=" << rigDistance << " legacyDistance2=" << legacyDistance;
                        }
                    }
                }
                line << '\n';
                return line.str();
            });

        // 실패 비교를 남긴 뒤 기존 Rig 거절 동작을 적용한다.
        if (rigQuery && rig.HasHits && rig.Owner == InvalidObjectId)
        {
            if (rig.AlreadyBroken)
            {
                m_Incoming.Status = IncomingJointStatus::AlreadyBroken;
                PatchLog("SP7b rejected: incoming=%u (NoBody=1 RigRoot=2 AlreadyBroken=3 NotRegistered=4)\n", unsigned(m_Incoming.Status));
            }
            return;
        }

        // Rig hit는 legacy 경계 구가 닿지 않아도 선택 owner로 계속 진행한다.
        const bool legacyAvailable = target != nullptr;
        if (!target && rigQuery && rig.Owner != InvalidObjectId)
        {
            target = scene.FindObject(rig.Owner);
            for (const auto& component : target->GetMeshComponents())
                for (size_t section = 0; section < component->GetGeometry()->GetSections().size(); ++section)
                    if (component->GetGeometry()->GetMesh(section).numJoints) mesh = component.get();
        }

        // 선택 실패와 compute pose 거절을 구분한다.
        if (!target || !mesh || !Renderer::UseComputePose)
        {
            PatchLog("Skeletal patch rejected: %s\n", !target ? "no skin target" : "compute pose disabled");
            if (diagnostic) throw std::runtime_error("Skeletal patch diagnostic target rejected");
            return;
        }
        auto selected = target == legacyTarget && mesh == legacyMesh ? std::move(legacy)
            : influence || !legacyAvailable ? SegmentJointSelection{}
            : SelectSegmentJoint(*target->GetModelInstance(), *mesh, target->GetTransform(), plane);

        // 실제 Rig 선택만 owner와 부모 마스크를 교체하고 Parent 진단 입력은 유지한다.
        if (rigQuery && rig.HasHits)
        {
            target = scene.FindObject(rig.Owner);
            for (const auto& component : target->GetMeshComponents())
            {
                bool skin = false;
                for (size_t section = 0; section < component->GetGeometry()->GetSections().size(); ++section)
                    skin |= component->GetGeometry()->GetMesh(section).numJoints != 0;
                if (skin && component->HasPoseBounds())
                {
                    mesh = component.get();
                    break;
                }
            }
            selected = rig.Selected;
            const auto& model = *target->GetModelInstance()->GetModel();
            for (size_t section = 0; section < mesh->GetGeometry()->GetSections().size(); ++section)
            {
                const auto& part = mesh->GetGeometry()->GetMesh(section);
                std::vector<uint32_t> mask(part.numJoints);
                for (uint32_t i = 0; i < part.numJoints; ++i)
                    mask[i] = model.m_JointIndices[part.startJoint + i] == selected.NodeId;
                selected.PaletteMasks.push_back(std::move(mask));
            }
        }

        if ((!influence && selected.NodeId == UINT32_MAX) || !target->GetModelInstance()->HasValidPose()
            || mesh->GetDeformBinding().Pose != PoseMode::FollowModel)
        {
            PatchLog("Skeletal patch rejected: no current skin joint pose (frozen or invalid input)\n");
            if (diagnostic) throw std::runtime_error("Skeletal patch diagnostic selection rejected");
            return;
        }

        // 선택된 component의 월드 변환을 한 번만 구해 시각화와 이후 절단이 함께 쓴다.
        const Matrix4 world(mesh->GetWorldTransform(target->GetTransform()));

        // 위상 아일랜드 시각화는 물리 등록과 GPU 절단 및 게시를 건너뛰고 snapshot만 만든다.
        if (TopologyIslandVizEnabled())
        {
            BuildTopologyIslandViz(*target, *mesh, world, plane);

            // 시연 캡처가 게시 신호를 기다리지 않도록 시각화 요청 프레임을 게시 프레임으로 알린다.
            if (m_Demo != 0)
            {
                m_CaptureRequest = m_Demo;
            }
            return;
        }

        // 명시 옵션에서만 최상위 교차 P 집합을 몸체별 준비 입력으로 고정한다.
        uint32_t multiCut = 0;
        CommandLineArgs::GetInteger(L"skeletal-multi-cut", multiCut);
        UnionCutRequest unionCut;
        if (multiCut && !influence && !m_OffsetCopy)
        {
            const Matrix4 sourceWorld(mesh->GetWorldTransform(target->GetTransform()));
            uint32_t unionChecks = 0;
            CommandLineArgs::GetInteger(L"skeletal-union-checks", unionChecks);
            if (unionChecks)
            {
                unionCut = CheckJointUnion(*mesh, sourceWorld,
                    target->GetModelInstance()->GetPoseParents(), target->GetName());
                if (!MeshCutting::TryBuildCutPlaneData({ 100, 100 },
                    AffineTransform(sourceWorld * Invert(unionCut.Plane.WorldToPlane)), plane))
                {
                    throw std::runtime_error("MI4h1 union world plane invalid");
                }

                // 이동·회전한 월드 평면을 다시 선택해 local 진단의 자식과 side 규약을 대조한다.
                const Matrix4 movedWorld = Matrix4(AffineTransform(Matrix3::MakeYRotation(0.7f), Vector3(7, -3, 5))) * sourceWorld;
                MeshCutting::CutPlaneData movedPlane;
                if (!MeshCutting::TryBuildCutPlaneData({ 100, 100 },
                    AffineTransform(movedWorld * Invert(unionCut.Plane.WorldToPlane)), movedPlane))
                {
                    throw std::runtime_error("MI4h1 transformed plane invalid");
                }
                const auto moved = SelectMultiCut(*mesh, movedWorld, target->GetModelInstance()->GetPoseParents(), movedPlane);
                if (moved.Roots != unionCut.Roots || moved.Children != unionCut.Children || moved.ChildSides != unionCut.ChildSides)
                {
                    throw std::runtime_error("MI4h1 transformed selection mismatch");
                }
                PatchLog("MI4h1 translated/rotated local selection roots/children/sides=PASS\n");
            }
            else
            {
                unionCut = SelectMultiCut(*mesh, sourceWorld,
                    target->GetModelInstance()->GetPoseParents(), plane);
            }
            m_MultiPrepare = unionCut.Roots.size() > 1;
            if (!m_MultiPrepare)
            {
                unionCut = {};
            }
        }
        const std::vector<uint32_t> cutRoots = m_MultiPrepare
            ? unionCut.Roots : std::vector<uint32_t>{ selected.NodeId };

        // D5는 AR과 같은 월드 joint 원점 거리로 모호한 요청을 먼저 거절한다.
        const bool boneCut = !m_MultiPrepare && rigQuery && rig.Reason == RigSelectionReason::D5;
        if (boneCut)
        {
            const auto* nodes = mesh->GetPreparedNodes();
            if (!nodes)
                return;
            size_t section = 0;
            while (!mesh->GetGeometry()->GetMesh(section).numJoints) ++section;
            const auto nodeWorld = Matrix4(mesh->GetWorldTransform(target->GetTransform()))
                * Invert(Matrix4(mesh->GetDeformBinding().SourceFrame.ToAffine()))
                * nodes[mesh->GetGeometry()->GetMesh(section).meshCBV] * nodes[selected.NodeId];
            if (std::abs(float(Dot(plane.WorldPlane, nodeWorld.GetW()))) <= 0.001f)
            {
                WritePatchLogSafely([&]
                {
                    // 진단 이름 조회 실패가 거절 동작을 바꾸지 않게 한다.
                    return "RigBoneCut rejected ambiguous node="
                        + SafeDiagnosticNodeName(target->GetName(), selected.NodeId) + "\n";
                });
                return;
            }

            // break snapshot과 분리하여 원본과 node 부착 배치를 저장한다.
            m_BoneOwner = target->GetId();
            m_SourceMesh = mesh->GetId();
            m_SourceRevision = mesh->GetGeometry();
            m_MaskRevision = mesh->GetTriangleMask() ? mesh->GetTriangleMask()->Revision : 0;
            m_BoneAttachment = { selected.NodeId, m_SourceMesh, nodeWorld,
                Matrix4(mesh->GetWorldTransform(target->GetTransform())) };
        }

        // Parent C는 현재 최종 pose로 최초 등록하고 incoming과 세대를 고정한다.
        if (physics && !influence && !boneCut)
        {
            if (!physics->IsSimulationEnabled())
            {
                PatchLog("SP7b rejected: physics off\n");
                return;
            }
            auto& skeletal = physics->GetSkeletalPhysics();
            RegisterAutoRig(scene, target->GetId(), skeletal);

            // 등록된 rig의 incoming 관절과 C 시점 세대를 저장한다.
            if (skeletal.IsRegistered(target->GetId()))
            {
                m_Incoming = skeletal.FindIncomingJoint(target->GetId(), m_MultiPrepare ? cutRoots.front() : selected.NodeId);
                if (m_Incoming.Status != IncomingJointStatus::Ok)
                {
                    PatchLog("SP7b rejected: incoming=%u (NoBody=1 RigRoot=2 AlreadyBroken=3 NotRegistered=4)\n", unsigned(m_Incoming.Status));
                    return;
                }
                m_Break = { target->GetId(), m_Incoming.JointId, skeletal.GetRigGeneration(target->GetId()),
                    skeletal.GetTopologyGeneration(target->GetId()) };

                // 다중 요청은 모든 incoming과 동일한 요청 시점 세대를 먼저 고정한다.
                if (m_MultiPrepare)
                {
                    for (const auto root : cutRoots)
                    {
                        const auto incoming = skeletal.FindIncomingJoint(target->GetId(), root);
                        if (incoming.Status != IncomingJointStatus::Ok)
                        {
                            PatchLog("SP7b rejected: incoming=%u (NoBody=1 RigRoot=2 AlreadyBroken=3 NotRegistered=4)\n", unsigned(incoming.Status));
                            m_MultiBreaks.clear();
                            return;
                        }
                        auto request = m_Break;
                        request.JointId = incoming.JointId;
                        m_MultiBreaks.push_back(request);
                    }
                }
                m_BreakReady = true;
            }
        }

        // 실제 절단은 등록 실패 시 원본과 화면을 그대로 보존한다.
        if (!m_OffsetCopy && !m_BreakReady && !boneCut)
        {
            PatchLog("SP7e rejected: no break target\n");
            return;
        }
        if (m_BreakReady && !m_OffsetCopy)
        {
            // D2가 보장하는 공통 skin frame을 사용하고 앞쪽 Static section은 건너뛴다.
            const auto& instance = *target->GetModelInstance();
            const auto* nodes = mesh->GetPreparedNodes();
            if (!nodes)
                return;
            const auto& parents = instance.GetPoseParents();
            size_t section = 0;
            while (!mesh->GetGeometry()->GetMesh(section).numJoints) ++section;
            const auto frame = Matrix4(mesh->GetWorldTransform(target->GetTransform()))
                * Invert(Matrix4(mesh->GetDeformBinding().SourceFrame.ToAffine()))
                * nodes[mesh->GetGeometry()->GetMesh(section).meshCBV];
            uint32_t sideNode = m_MultiPrepare ? cutRoots.front() : selected.NodeId;
            if (!m_MultiPrepare && selected.Kind == SegmentJointKind::Leaf)
            {
                sideNode = parents[sideNode];
                while (sideNode != UINT32_MAX && !instance.GetModel()->m_InfluencingJoints[sideNode])
                    sideNode = parents[sideNode];
            }
            if (sideNode == UINT32_MAX || parents[m_MultiPrepare ? cutRoots.front() : selected.NodeId] == UINT32_MAX)
            {
                PatchLog("SP7e rejected: no influencing ancestor\n");
                return;
            }
            const float side = Math::Dot(frame * nodes[sideNode].GetW(), plane.WorldPlane);
            const uint32_t other = m_MultiPrepare ? unionCut.Children.front()
                : selected.Kind == SegmentJointKind::Segment ? selected.ChildNodeId : selected.NodeId;
            const float opposite = Math::Dot(frame * nodes[other].GetW(), plane.WorldPlane);
            if (!m_MultiPrepare && selected.Kind == SegmentJointKind::Leaf ? std::abs(side) <= 0.001f
                : std::abs(side) <= 0.001f && std::abs(opposite) <= 0.001f)
            {
                PatchLog("SP7e rejected: ambiguous side within 1mm\n");
                return;
            }
            m_BodySides = { side >= 0 ? 0u : 1u, side >= 0 ? 1u : 0u };
            m_SourceMesh = mesh->GetId();
            m_SourceRevision = mesh->GetGeometry();
            m_MaskRevision = mesh->GetTriangleMask() ? mesh->GetTriangleMask()->Revision : 0;
            const auto groups = BuildCutBodyGroups(parents, {}, cutRoots);
            if (groups.AttachNodes.empty())
                return;
            m_Attachments.resize(groups.AttachNodes.size());
            m_PieceIds.assign(groups.AttachNodes.size(), 0);
            for (size_t body = 0; body < groups.AttachNodes.size(); ++body)
            {
                // 각 몸체는 같은 제출 포즈와 원본 배치를 사용한다.
                const uint32_t node = groups.AttachNodes[body];
                m_Attachments[body] = { node, m_SourceMesh, frame * nodes[node],
                    Math::Matrix4(mesh->GetWorldTransform(target->GetTransform())) };
            }
        }

        // 추출과 같은 최종 pose에서 A/B 또는 D5의 전체 팔레트와 고정 집합을 저장한다.
        if (!influence && (boneCut || (m_BreakReady && !m_OffsetCopy)))
        {
            const auto& instance = *target->GetModelInstance();
            const auto& model = *instance.GetModel();
            const auto* nodes = mesh->GetPreparedNodes();
            const auto* joints = mesh->GetPreparedJoints();
            if (!nodes || !joints)
                return;
            const auto& parents = instance.GetPoseParents();
            const Matrix4 sourceInverse(Invert(mesh->GetDeformBinding().SourceFrame.ToAffine()));
            for (size_t s = 0; s < mesh->GetGeometry()->GetSections().size(); ++s)
                m_SkinSectionNodes.push_back(sourceInverse * nodes[mesh->GetGeometry()->GetMesh(s).meshCBV]);
            const std::vector<uint32_t> jointNodes(model.m_JointIndices.get(), model.m_JointIndices.get() + model.m_NumJoints);
            const auto groups = BuildCutBodyGroups(parents, jointNodes, cutRoots);
            if (groups.AttachNodes.empty())
                return;

            // A 패치와 같은 요청 시점의 물리·애니메이션 입력을 성공 게시까지 보관한다.
            if (!boneCut)
            {
                m_PendingDetachRoots = cutRoots;
                m_PendingDetach.Owner = target->GetId();
                m_PendingDetach.SourceModel = instance.GetModel();
                m_PendingDetach.RootNode = cutRoots.front();
                m_PendingDetach.Palette.assign(joints, joints + model.m_NumJoints);
                m_PendingDetach.Nodes.assign(nodes, nodes + model.m_NumNodes);
                if (mesh->GetPreparedAnimJoints() && mesh->GetPreparedAnimNodes())
                {
                    m_PendingDetach.AnimPalette.assign(mesh->GetPreparedAnimJoints(), mesh->GetPreparedAnimJoints() + model.m_NumJoints);
                    m_PendingDetach.AnimNodes.assign(mesh->GetPreparedAnimNodes(), mesh->GetPreparedAnimNodes() + model.m_NumNodes);
                }
            }

            // 몸체별 snapshot과 검증 입력을 준비한다.
            m_SkinSnapshots.resize(boneCut ? 1 : groups.AttachNodes.size());
            m_SkinPaletteErrors.assign(m_SkinSnapshots.size(), 0);
            m_RemainLengthTables.assign(m_SkinSnapshots.size(), {});
            m_PatchBoundaryFlags.clear();
            uint32_t checks = 0;
            CommandLineArgs::GetInteger(L"skeletal-patch-break-checks", checks);
            m_SkinCheckCase = checks ? m_BreakCheck + 1 : 0;

            // joint 인덱스를 변환기의 NodeId로 바꾼 뒤 P를 포함한 자손 여부를 판정한다.
            for (uint32_t side = 0; side < m_SkinSnapshots.size(); ++side)
            {
                auto& snapshot = m_SkinSnapshots[side];
                snapshot = {};
                snapshot.SourceRevision = mesh->GetGeometry()->GetId();
                snapshot.SourceFrame = Matrix4(mesh->GetDeformBinding().SourceFrame.ToAffine());
                snapshot.SourceFrozen = mesh->GetDeformBinding().Pose == PoseMode::Frozen;
                snapshot.AttachNode = boneCut ? selected.NodeId : groups.AttachNodes[side];
                snapshot.AttachAtCut = nodes[snapshot.AttachNode];
                size_t firstSkin = 0;
                while (!mesh->GetGeometry()->GetMesh(firstSkin).numJoints) ++firstSkin;
                snapshot.MeshNodeAtCut = m_SkinSectionNodes[firstSkin];
                snapshot.JointsAtCut.assign(joints, joints + model.m_NumJoints);
                snapshot.Frozen = boneCut ? std::vector<uint8_t>(model.m_NumJoints, 0)
                    : groups.FrozenPalettes[side];

                // 몸통 쪽이면 애니메이션 추종 플래그와 O_k를 채운다.
                FillAnimFollow(snapshot, *mesh, parents, groups, side, boneCut);

                // 체크 실행에서만 실제 t_cut 입력으로 팔레트 오차를 계산한다.
                m_SkinPaletteErrors[side] = 0;
                if (m_SkinCheckCase)
                {
                    CheckAnimFollowAtCut(snapshot, *mesh);
                    std::vector<Joint> palette;
                    BuildPatchPalette(snapshot, joints, nodes, palette, mesh->GetPreparedAnimJoints(), mesh->GetPreparedAnimNodes(), PatchAnimFollowEnabled());
                    for (uint32_t k = 0; k < model.m_NumJoints; ++k)
                    {
                        const Matrix4 actual[] = { palette[k].posXform, Matrix4(palette[k].nrmXform) };
                        const Matrix4 expected[] = { joints[k].posXform, Matrix4(joints[k].nrmXform) };
                        for (uint32_t matrix = 0; matrix < 2; ++matrix)
                        {
                            DirectX::XMFLOAT4X4 a, b;
                            DirectX::XMStoreFloat4x4(&a, actual[matrix]);
                            DirectX::XMStoreFloat4x4(&b, expected[matrix]);
                            for (uint32_t r = 0; r < 4; ++r)
                                for (uint32_t c = 0; c < 4; ++c)
                                {
                                    const float error = std::abs(a.m[r][c] - b.m[r][c]);
                                    m_SkinPaletteErrors[side] = !std::isfinite(error) ? INFINITY
                                        : std::max(m_SkinPaletteErrors[side], error);
                                }
                        }
                    }
                }
            }

            // 비교 데모는 모드 0도 같은 LW 계산으로 비율을 기록하고, 재분배는 기존 옵션을 따른다.
            uint32_t lengthDemo = 0;
            CommandLineArgs::GetInteger(L"skeletal-break-demo", lengthDemo);
            if (!boneCut && (PatchLengthWeightsMode() || PatchLengthBlendEnabled() || lengthDemo || m_PendingDetach.RootNode != UINT32_MAX)
                && m_RemainLengthTables.size() == m_SkinSnapshots.size())
            {
                // 절단 시점 pose의 월드 joint 위치를 구한다(snapshot의 JointsAtCut과 같은 pose).
                size_t firstSkin = 0;
                while (firstSkin < mesh->GetGeometry()->GetSections().size() && !mesh->GetGeometry()->GetMesh(firstSkin).numJoints)
                    ++firstSkin;
                const Matrix4 toWorld = Matrix4(mesh->GetWorldTransform(target->GetTransform())) * m_SkinSectionNodes[firstSkin];
                std::vector<Math::Vector3> positions(parents.size());
                for (uint32_t node = 0; node < parents.size(); ++node)
                    positions[node] = Math::Vector3(toWorld * nodes[node] * Math::Vector4(0, 0, 0, 1));

                // P_i별 부모·자식 노드를 모은다(단일은 selected, 다중은 unionCut).
                std::vector<uint32_t> cutParents, cutChildren;
                if (m_MultiPrepare)
                {
                    cutParents = unionCut.Roots;
                    cutChildren = unionCut.Children;
                }
                else
                {
                    cutParents = { selected.NodeId };
                    cutChildren = { selected.Kind == SegmentJointKind::Segment ? selected.ChildNodeId : UINT32_MAX };
                }

                // 단일·다중 모두 절단 시점 선분과 평면의 교점으로 r_P를 구한다(a/(a-b), [0,1]). 범위 밖 노드는 선분 없음이다.
                std::vector<RemainLengthCut> cuts;
                for (size_t i = 0; i < cutParents.size(); ++i)
                {
                    RemainLengthCut cut;
                    cut.NodeId = cutParents[i];
                    cut.ChildNodeId = i < cutChildren.size() ? cutChildren[i] : UINT32_MAX;
                    if (cut.NodeId >= positions.size() || cut.ChildNodeId >= positions.size())
                        cut.ChildNodeId = UINT32_MAX;
                    cut.Intersection = cut.NodeId < positions.size() ? positions[cut.NodeId] : Math::Vector3(Math::kZero);
                    if (cut.ChildNodeId != UINT32_MAX)
                    {
                        const auto& from = positions[cut.NodeId];
                        const auto& to = positions[cut.ChildNodeId];
                        const float a = Math::Dot(plane.WorldPlane, Math::Vector4(from, 1));
                        const float b = Math::Dot(plane.WorldPlane, Math::Vector4(to, 1));
                        const float fraction = a != b ? std::min(std::max(a / (a - b), 0.0f), 1.0f) : 1.0f;
                        cut.Intersection = from + (to - from) * fraction;
                    }
                    cuts.push_back(cut);
                }
                auto lengthTable = BuildRemainLengthTable(cuts, positions, jointNodes, groups.NodeGroups);

                // 옵션과 무관하게 이번 root의 원래 길이 비율을 이력 입력에 남긴다.
                m_PendingDetach.LengthRatio = lengthTable.Ratio;

                // LB는 원래 비율 표를 snapshot에 보관하고 LW 모드 0·1의 P=1 처리는 별도로 유지한다.
                if (PatchLengthBlendEnabled())
                {
                    m_SkinSnapshots[0].LengthBlendRatio = lengthTable.Ratio;

                    // 절단 요청 시점의 실제 입력으로 혼합 팔레트 항등성을 검사한다.
                    std::vector<Joint> palette;
                    BuildPatchPalette(m_SkinSnapshots[0], joints, nodes, palette,
                        mesh->GetPreparedAnimJoints(), mesh->GetPreparedAnimNodes(), PatchAnimFollowEnabled());
                    PatchLog("%s", CheckLengthBlendPalette(m_SkinSnapshots[0], palette, nodes,
                        mesh->GetPreparedAnimJoints(), mesh->GetPreparedAnimNodes(), "A", -1).c_str());

                    // 합성 C≠I 입력에서도 부착 노드의 C와 D를 중복 적용하지 않는지 검사한다.
                    std::vector<Matrix4> movedNodes(nodes, nodes + parents.size());
                    movedNodes[m_SkinSnapshots[0].AttachNode] = Matrix4(Math::AffineTransform(
                        Math::Matrix3(Math::kIdentity), Math::Vector3(0.25f, -0.5f, 0.75f))) * movedNodes[m_SkinSnapshots[0].AttachNode];
                    BuildPatchPalette(m_SkinSnapshots[0], joints, movedNodes.data(), palette,
                        mesh->GetPreparedAnimJoints(), mesh->GetPreparedAnimNodes(), PatchAnimFollowEnabled());
                    PatchLog("%s", CheckLengthBlendPalette(m_SkinSnapshots[0], palette, movedNodes.data(),
                        mesh->GetPreparedAnimJoints(), mesh->GetPreparedAnimNodes(), "A-C!=I", -2).c_str());
                }
                if (PatchLengthWeightsMode() != 2)
                    for (size_t k = 0; k < lengthTable.Ratio.size(); ++k)
                        if (lengthTable.CutRoot[k] >= 0 && !lengthTable.Descendant[k])
                            lengthTable.Ratio[k] = 1.0f;

                // 소속 P_i가 있는 joint마다 같은 section 팔레트 범위 안에서 P_i의 NodeId를 찾아 합 0 폴백 인덱스로 정한다(없으면 -1).
                for (size_t k = 0; k < jointNodes.size(); ++k)
                {
                    if (lengthTable.CutRoot[k] < 0)
                        continue;
                    for (size_t s = 0; s < mesh->GetGeometry()->GetSections().size(); ++s)
                    {
                        const auto& sectionMesh = mesh->GetGeometry()->GetMesh(s);
                        if (k < sectionMesh.startJoint || k >= size_t(sectionMesh.startJoint) + sectionMesh.numJoints)
                            continue;
                        for (size_t j = sectionMesh.startJoint; j < size_t(sectionMesh.startJoint) + sectionMesh.numJoints && j < jointNodes.size(); ++j)
                            if (jointNodes[j] == cuts[lengthTable.CutRoot[k]].NodeId)
                            {
                                lengthTable.Fallback[k] = int(j);
                                break;
                            }
                        break;
                    }
                }

                // P_i마다 r_P와 자손 joint 수를 기록하고, 단일 절단은 선택 시점 교점과의 pose 차이도 남긴다.
                const auto& table = lengthTable;
                for (size_t i = 0; i < cuts.size(); ++i)
                {
                    std::set<uint32_t> descendants;
                    for (size_t k = 0; k < jointNodes.size(); ++k)
                        if (table.CutRoot[k] == int(i) && table.Descendant[k])
                            descendants.insert(jointNodes[k]);
                    float length = 0.0f, ratio = 1.0f;
                    if (cuts[i].ChildNodeId != UINT32_MAX)
                    {
                        length = float(Math::Length(positions[cuts[i].ChildNodeId] - positions[cuts[i].NodeId]));
                        ratio = length > 1e-9f ? float(Math::Length(cuts[i].Intersection - positions[cuts[i].NodeId])) / length : 1.0f;
                    }
                    PatchLog("LW1 mode=%d P=%u child=%u r_P=%.4f segmentLen=%.4f descendantJoints=%zu\n",
                        PatchLengthWeightsMode(), cuts[i].NodeId, cuts[i].ChildNodeId, ratio, length, descendants.size());

                    // 단일·다중·잎 본 모두 같은 로그를 쓰고 중복 section joint의 저장값도 확인한다.
                    if (PatchLengthBlendEnabled())
                    {
                        uint32_t rootEntries = 0, mismatches = 0;
                        const auto& ratios = m_SkinSnapshots[0].LengthBlendRatio;
                        for (size_t k = 0; k < jointNodes.size(); ++k)
                            if (jointNodes[k] == cuts[i].NodeId)
                            {
                                ++rootEntries;
                                const float expected = table.Descendant[k] ? 0.0f : std::min(std::max(ratio, 0.0f), 1.0f);
                                mismatches += ratios[k] != expected;
                            }
                        PatchLog("LB1 P=%u r_P=%.4f descendants=%u paletteEntries=%u duplicates=%u match=%s\n",
                            cuts[i].NodeId, ratio, uint32_t(descendants.size()), rootEntries,
                            rootEntries ? rootEntries - 1 : 0, !rootEntries ? "N/A" : mismatches ? "FAIL" : "PASS");
                    }

                    if (!m_MultiPrepare && cuts[i].ChildNodeId != UINT32_MAX)
                    {
                        const float planeErr = Math::Dot(plane.WorldPlane, Math::Vector4(selected.Intersection, 1));
                        const float selectionRatio = length > 1e-9f
                            ? std::min(float(Math::Length(selected.Intersection - positions[cuts[i].NodeId])) / length, 1.0f) : 1.0f;
                        PatchLog("LW1 selection planeErr_mm=%.6f selFracDiff=%.6f\n", planeErr * 1000.0f, selectionRatio - ratio);
                    }
                }

                // 표의 자손 집합이 Frozen이면서 P_i 자신이 아닌 joint 집합과 같은지 확인한다.
                bool descendantMatch = table.Descendant.size() == m_SkinSnapshots[0].Frozen.size();
                for (size_t k = 0; descendantMatch && k < table.Descendant.size(); ++k)
                {
                    const bool isRoot = std::find(cutRoots.begin(), cutRoots.end(), jointNodes[k]) != cutRoots.end();
                    descendantMatch = (table.Descendant[k] != 0) == (m_SkinSnapshots[0].Frozen[k] != 0 && !isRoot);
                }
                PatchLog("LW1 table size=%zu descendantMatch=%s\n", table.Ratio.size(), descendantMatch ? "PASS" : "FAIL");

                // 모드 0의 표는 진단에만 사용하고 GPU 재분배 경로로 넘기지 않는다.
                if (PatchLengthWeightsMode())
                    m_RemainLengthTables[0] = std::move(lengthTable);
            }
        }

        // 현재 GPU 입력과 일치하는 유효 선택만 segment 또는 말단으로 기록한다.
        if (!influence) PatchLog("Skeletal patch target=%s NodeId=%u kind=%s child=%u intersection=(%.6f, %.6f, %.6f)\n",
            target->GetName().c_str(), selected.NodeId, selected.Kind == SegmentJointKind::Segment ? "segment" : "leaf",
            selected.ChildNodeId, float(selected.Intersection.GetX()), float(selected.Intersection.GetY()), float(selected.Intersection.GetZ()));

        // 모델 이름은 진단 파일 조회에만 사용하고 선택·추출 조건에는 사용하지 않는다.
        if (diagnostic && !influence)
            WritePatchLogSafely([&]
            {
                // 이름과 출력 예외가 Parent와 Rig 요청을 중단하지 않게 한다.
                std::ostringstream line;
                line << "Skeletal patch selected model=" << target->GetName() << " NodeId=" << selected.NodeId
                    << " name=" << SafeDiagnosticNodeName(target->GetName(), selected.NodeId)
                    << " kind=" << (selected.Kind == SegmentJointKind::Segment ? "segment" : "leaf")
                    << " child=" << selected.ChildNodeId << '\n';
                return line.str();
            });

        // cutter와 같은 component-local 평면 변환을 사용한다.
        auto local = plane;
        local.WorldPlane = Transpose(world) * plane.WorldPlane;
        local.WorldToPlane = plane.WorldToPlane * world;
        // 제출 시점 component world를 고정하고 폭은 GPU의 posed 월드 X 경계로 받는다.
        if (!SceneTransform::TryFromAffine(mesh->GetWorldTransform(target->GetTransform()), m_World))
            throw std::runtime_error("Skeletal patch source world is not TRS");
        m_ModelName = target->GetName();
        m_Name = m_ModelName + "_SkeletalPatch";
        m_Diagnostic = diagnostic;
        m_Plane = plane;
        // 명시한 옵션에서만 별도 합집합 추출을 검증하고 형제 P_i 쌍을 받는다.
        uint32_t unionChecks = 0;
        if (!m_MultiPrepare && CommandLineArgs::GetInteger(L"skeletal-union-checks", unionChecks) && unionChecks)
            unionCut = CheckJointUnion(*mesh, world, target->GetModelInstance()->GetPoseParents(), m_ModelName);

        // 합집합 진단은 형제 평면을 월드로 옮기고 n+1 몸체의 폴백 반공간을 정한다.
        if (!unionCut.Roots.empty())
        {
            if (!MeshCutting::TryBuildCutPlaneData({ 100, 100 },
                AffineTransform(world * Invert(unionCut.Plane.WorldToPlane)), m_Plane))
                throw std::runtime_error("MI4d2 union world plane invalid");
            local = m_Plane;
            local.WorldPlane = Transpose(world) * m_Plane.WorldPlane;
            local.WorldToPlane = m_Plane.WorldToPlane * world;
            m_BodySides = { 1 - unionCut.ChildSides[0] };
            m_BodySides.insert(m_BodySides.end(), unionCut.ChildSides.begin(), unionCut.ChildSides.end());
            PatchLog("MI4d2 union cut P=%u,%u child=%u,%u childSide=%u,%u bodies=3 publish/break/mask=skipped\n",
                unionCut.Roots[0], unionCut.Roots[1], unionCut.Children[0], unionCut.Children[1],
                unionCut.ChildSides[0], unionCut.ChildSides[1]);
        }

        // 본 선택까지 끝난 시각을 성능 로그에 남긴다.
        CutPerf::Mark("select-done");

        // 기본 실행은 단일 P를, 합집합 진단은 P_i 배열과 자식 snapshot을 제출한다.
        const bool submitted = unionCut.Roots.empty()
            ? m_Job.Submit(*mesh, local, selected.PaletteMasks, world, selected.NodeId, diagnostic, influence,
                target->GetModelInstance()->GetPoseParents(), selected.ChildNodeId)
            : m_Job.Submit(*mesh, local, {}, world, unionCut.Roots[0], diagnostic, false,
                target->GetModelInstance()->GetPoseParents(), UINT32_MAX, unionCut.Roots, unionCut.Children);

        // 합집합 제출이 거절되면 몸체 side를 기본 두 몸체로 되돌린다.
        if (!submitted && !unionCut.Roots.empty())
        {
            m_BodySides = { 0, 1 };
        }
        CutPerf::Mark("extract-submitted");
        m_BonePending = boneCut && submitted;
        if (!submitted && diagnostic)
            throw std::runtime_error("Skeletal patch diagnostic submit rejected");
    }

    // 절단 없이 격자 위상 맵을 평면으로 갈라 아일랜드별 색 월드 선분 snapshot과 요약 로그를 만든다.
    // 정점 스키닝은 DiagnoseIslands와 같고, 평면은 스키닝 경계에 맞추며 높이 옵션이면 수평으로 바꾼다.
    void SkeletalPatchCopy::BuildTopologyIslandViz(const GameObject& target, const MeshComponent& mesh,
        const Math::Matrix4& world, const MeshCutting::CutPlaneData& requestedPlane)
    {
        // 스키닝 시간 측정을 시작하고 요청 평면을 복사해 둔다. 아래에서 필요하면 평면을 교체한다.
        const double startMs = TopologyNowMs();
        MeshCutting::CutPlaneData plane = requestedPlane;

        // 이전 snapshot을 비우고 준비된 포즈가 없으면 로그만 남기고 건너뛴다.
        m_TopologyViz.clear();
        const auto& geometry = *mesh.GetGeometry();
        const auto& model = *geometry.GetSource();
        const auto* joints = mesh.GetPreparedJoints();
        const auto* nodes = mesh.GetPreparedNodes();
        if (!joints || !nodes)
        {
            PatchLog("TIV skipped: prepared pose is not ready\n");
            return;
        }

        // SkeletalPatchJob::Submit과 같은 방식으로 원본 mesh의 skin 번호를 센다.
        std::vector<uint32_t> skinSections(model.m_NumMeshes, UINT32_MAX);
        uint32_t skinCount = 0;
        const uint8_t* meshData = model.m_MeshData.get();
        for (uint32_t s = 0; s < model.m_NumMeshes; ++s)
        {
            const auto& part = *reinterpret_cast<const Mesh*>(meshData);
            if (part.numJoints)
            {
                skinSections[s] = skinCount;
                ++skinCount;
            }
            meshData += sizeof(Mesh) + (part.numDraws - 1) * sizeof(Mesh::Draw);
        }

        // 컴포넌트의 각 skin section에 제출 프레임(SourceFrame 역행렬 * 노드)을 대응시킨다.
        const Math::Matrix4 sourceInverse(Invert(Math::Matrix4(mesh.GetDeformBinding().SourceFrame.ToAffine())));
        const size_t skinTotal = model.m_SkinCpuSections.size();
        std::vector<Math::Matrix4> frames(skinTotal, Math::Matrix4(Math::kIdentity));
        std::vector<uint8_t> hasFrame(skinTotal, 0);
        for (uint32_t s = 0; s < geometry.GetSections().size(); ++s)
        {
            // section의 원본 skin 번호를 찾고 skin이 아니면 건너뛴다.
            const uint32_t skin = skinSections.at(geometry.GetSections()[s].SourceMesh);
            if (skin == UINT32_MAX || skin >= skinTotal)
            {
                continue;
            }
            frames[skin] = sourceInverse * nodes[geometry.GetMesh(s).meshCBV];
            hasFrame[skin] = 1;
        }

        // 포즈 프레임이 없는 skin section 수를 센다. 셀 방식은 모든 section의 포즈가 필요하다.
        uint32_t unposedSections = 0;
        for (uint32_t skin = 0; skin < skinTotal; ++skin)
        {
            if (!hasFrame[skin])
            {
                ++unposedSections;
            }
        }

        // 모든 skin 정점을 스키닝해 월드 위치와 전체 경계를 구한다.
        std::vector<std::vector<Math::Vector3>> positions(skinTotal);
        std::vector<std::vector<float>> distances(skinTotal);
        std::vector<std::vector<int8_t>> signs(skinTotal);
        std::vector<std::array<uint32_t, 2>> triangles;
        Math::Vector3 boundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
        Math::Vector3 boundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        for (uint32_t skin = 0; skin < skinTotal; ++skin)
        {
            if (!hasFrame[skin])
            {
                continue;
            }

            // section 정점 수만큼 결과 배열을 준비한다.
            const auto& section = model.m_SkinCpuSections[skin];
            positions[skin].resize(section.size());
            distances[skin].resize(section.size());
            signs[skin].resize(section.size());

            // 정점마다 스키닝과 평면 부호를 계산한다.
            for (size_t v = 0; v < section.size(); ++v)
            {
                // 정규화한 weight로 팔레트 변환을 섞는다.
                const auto& vertex = section[v];
                Math::Vector4 skinned(Math::kZero);
                uint32_t sum = 0;

                // weight 합을 구한 뒤 팔레트 변환을 가중 합산한다.
                for (uint32_t weight : vertex.Weights)
                {
                    sum += weight;
                }
                for (uint32_t j = 0; j < 4; ++j)
                {
                    if (vertex.Weights[j])
                    {
                        skinned = skinned + (joints[section.StartJoint + vertex.Joints[j]].posXform
                            * Math::Vector4(vertex.Position[0], vertex.Position[1], vertex.Position[2], 1))
                            * (float(vertex.Weights[j]) / std::max(sum, 1u));
                    }
                }

                // 제출 프레임과 컴포넌트 월드를 적용한 위치를 저장하고 경계를 넓힌다.
                const Math::Vector3 local(frames[skin] * skinned);
                const Math::Vector3 position(world * Math::Vector4(local, 1));
                positions[skin][v] = position;
                boundsMin = Math::Min(boundsMin, position);
                boundsMax = Math::Max(boundsMax, position);
            }

            // 이 section의 모든 삼각형을 아일랜드 판정 대상으로 넣는다.
            const uint32_t triangleCount = uint32_t(model.m_LatticeTopology.Indices.at(skin).size() / 3);
            for (uint32_t tri = 0; tri < triangleCount; ++tri)
            {
                triangles.push_back({ skin, tri });
            }
        }

        // 누적된 정점이 없으면 경계가 비어 있으므로 로그만 남기고 건너뛴다.
        if (float(boundsMin.GetX()) > float(boundsMax.GetX()))
        {
            PatchLog("TIV skipped: empty bounds\n");
            return;
        }

        // 높이 옵션이 있으면 경계 높이 비율의 수평 월드 평면(법선 +Y)으로 요청 평면을 바꾼다.
        float heightRatio = 0.0f;
        if (TopologyIslandVizHeightRatio(heightRatio))
        {
            const float minY = boundsMin.GetY();
            const float maxY = boundsMax.GetY();
            const float height = minY + heightRatio * (maxY - minY);
            plane.WorldPlane = Math::Vector4(0.0f, 1.0f, 0.0f, -height);
            PatchLog("TIV horizontal plane ratio=%.3f height=%.4f minY=%.4f maxY=%.4f\n",
                heightRatio, height, minY, maxY);
        }

        // 경계 중심을 평면에 투영한 점을 사각형 중심으로 삼는다.
        // 사각형 크기는 경계가 평면 두 축에 드리우는 크기에 15% 여유를 둔 값이다.
        const Math::Vector3 normal(plane.WorldPlane);
        const Math::Vector3 boundsCenter = (boundsMin + boundsMax) * 0.5f;
        const Math::Vector3 boundsHalf = (boundsMax - boundsMin) * 0.5f;
        const float centerDistance = Math::Dot(plane.WorldPlane, Math::Vector4(boundsCenter, 1));
        const Math::Vector3 planeCenter = boundsCenter - normal * centerDistance;
        const Math::Vector3 reference = std::abs(float(normal.GetY())) > 0.9f
            ? Math::Vector3(1.0f, 0.0f, 0.0f) : Math::Vector3(0.0f, 1.0f, 0.0f);
        const Math::Vector3 axisU = Math::Normalize(Math::Cross(normal, reference));
        const Math::Vector3 axisV = Math::Cross(normal, axisU);
        const float halfU = 1.15f * (std::abs(float(axisU.GetX())) * float(boundsHalf.GetX())
            + std::abs(float(axisU.GetY())) * float(boundsHalf.GetY())
            + std::abs(float(axisU.GetZ())) * float(boundsHalf.GetZ()));
        const float halfV = 1.15f * (std::abs(float(axisV.GetX())) * float(boundsHalf.GetX())
            + std::abs(float(axisV.GetY())) * float(boundsHalf.GetY())
            + std::abs(float(axisV.GetZ())) * float(boundsHalf.GetZ()));
        plane.WorldCorners[0] = planeCenter - axisU * halfU - axisV * halfV;
        plane.WorldCorners[1] = planeCenter + axisU * halfU - axisV * halfV;
        plane.WorldCorners[2] = planeCenter + axisU * halfU + axisV * halfV;
        plane.WorldCorners[3] = planeCenter - axisU * halfU + axisV * halfV;

        // 최종 평면 기준으로 정점의 부호 거리와 1e-5 규칙의 부호를 구한다.
        for (uint32_t skin = 0; skin < skinTotal; ++skin)
        {
            for (size_t v = 0; v < positions[skin].size(); ++v)
            {
                const float distance = Math::Dot(plane.WorldPlane, Math::Vector4(positions[skin][v], 1));
                distances[skin][v] = distance;
                signs[skin][v] = std::abs(distance) <= 1e-5f ? 0 : distance > 0 ? 1 : -1;
            }
        }
        const double skinMs = TopologyNowMs() - startMs;

        // 시각화 옵션을 읽는다.
        uint32_t method = TopologyIslandVizMethod();
        const uint32_t neighbors = TopologyIslandVizNeighbors();
        bool drawGraph = TopologyIslandVizDrawGraph();

        // 그래프 그리기는 셀 방식 데이터가 필요하므로 메시 방식이면 셀 방식으로 바꾼다.
        if (drawGraph && method == 0)
        {
            PatchLog("TIV draw=1 needs method=1, falling back to method 1\n");
            method = 1;
        }

        // 포즈 없는 section이 있으면 셀 방식을 건너뛰고 요청이 셀 방식이면 메시 방식으로 되돌린다.
        const bool useCellMethod = unposedSections == 0;
        if (!useCellMethod)
        {
            PatchLog("TIV cell method skipped: unposed sections=%u\n", unposedSections);
            if (method == 1 || drawGraph)
            {
                PatchLog("TIV cell method unavailable, falling back to method 0\n");
                method = 0;
                drawGraph = false;
            }
        }

        // 메시 방식 라벨링 시간만 잰다. 몸체 투표는 쓰지 않고 아일랜드 연결만 구한다.
        const double meshStart = TopologyNowMs();
        const auto labels = LabelPatchIslands(model.m_LatticeTopology, model.m_SkinCpuSections,
            triangles, signs, [](uint32_t) { return 0u; });
        const double meshMs = TopologyNowMs() - meshStart;

        // 셀 방식이 가능하면 그래프를 캐시에서 가져온다. 그래프 빌드는 비교 시간에서 제외한다.
        const LatticeCellGraph* graph = nullptr;
        if (useCellMethod)
        {
            graph = &TopologyCellGraph(&model, model.m_LatticeTopology, model.m_SkinCpuSections);
        }

        // 셀 방식 라벨링 시간만 잰다. 가능하면 메시 방식과 항상 함께 실행해 비교한다.
        std::vector<int8_t> cellSides;
        CellIslandLabels cellLabels;
        std::vector<std::vector<uint32_t>> vertexIsland;
        double cellMs = 0.0;
        if (useCellMethod)
        {
            const double cellStart = TopologyNowMs();
            cellSides = CellSidesFromVertexDistances(*graph, distances);
            cellLabels = LabelLatticeCellIslands(*graph, cellSides, neighbors);
            vertexIsland = VertexIslands(*graph, cellLabels);
            cellMs = TopologyNowMs() - cellStart;
        }

        // 스키닝과 두 방식의 라벨링 시간을 기록한다.
        PatchLog("TIV skin ms=%.2f\n", skinMs);
        PatchLog("TIV timing mesh_ms=%.2f cell_ms=%.2f (cell excludes graph build) mesh_islands=%u cell_islands=%u\n",
            meshMs, cellMs, labels.IslandCount, cellLabels.IslandCount);

        // 메시 방식 아일랜드를 큰 순서로 색 순위를 정한다.
        std::vector<uint32_t> order(labels.Islands.size());
        std::iota(order.begin(), order.end(), 0u);
        std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b)
        {
            // 삼각형이 많은 아일랜드가 앞선다.
            return labels.Islands[a].Triangles > labels.Islands[b].Triangles;
        });
        std::vector<uint32_t> rank(order.size());
        for (uint32_t i = 0; i < order.size(); ++i)
        {
            rank[order[i]] = i;
        }

        // 셀 방식이면 메시 방식 삼각형 순회를 건너뛰고 아래에서 셀 아일랜드 색으로 그린다.
        // 메시 방식이면 삼각형마다 각 side 아일랜드에 속한 부분의 간선을 그 색의 선분으로 만든다.
        std::set<std::array<uint32_t, 5>> emitted;
        const bool useMeshMethod = method == 0;
        const size_t meshTriangleCount = useMeshMethod ? triangles.size() : 0;
        for (size_t t = 0; t < meshTriangleCount; ++t)
        {
            const uint32_t skin = triangles[t][0];
            const uint32_t tri = triangles[t][1];
            const auto& indices = model.m_LatticeTopology.Indices[skin];
            for (uint32_t side = 0; side < 2; ++side)
            {
                const uint32_t island = labels.TriangleSideIsland[t][side];
                if (island == UINT32_MAX)
                {
                    continue;
                }
                const Color color = TopologyIslandColor(rank[island]);
                for (uint32_t e = 0; e < 3; ++e)
                {
                    // 부호 0은 양쪽 side에 속하므로 같은 side 판정에 포함한다.
                    const uint32_t a = indices[tri * 3 + e];
                    const uint32_t b = indices[tri * 3 + (e + 1) % 3];
                    const bool inA = side == 0 ? signs[skin][a] >= 0 : signs[skin][a] <= 0;
                    const bool inB = side == 0 ? signs[skin][b] >= 0 : signs[skin][b] <= 0;
                    if (!inA && !inB)
                    {
                        continue;
                    }
                    Math::Vector3 from = positions[skin][a];
                    Math::Vector3 to = positions[skin][b];
                    std::array<uint32_t, 5> key = { skin, std::min(a, b), std::max(a, b), island, 0 };

                    // 한 끝만 이 side이면 평면 교점까지의 부분 선분만 낸다. 끝점이 평면 위면 길이 0이라 건너뛴다.
                    if (!inA || !inB)
                    {
                        const uint32_t inside = inA ? a : b;
                        const uint32_t outside = inA ? b : a;
                        if (signs[skin][inside] == 0)
                        {
                            continue;
                        }
                        const float inDistance = distances[skin][inside];
                        const float fraction = inDistance / (inDistance - distances[skin][outside]);
                        from = positions[skin][inside];
                        to = from + (positions[skin][outside] - from) * fraction;
                        key = { skin, inside, outside, island, 1 };
                    }

                    // 같은 아일랜드의 같은 선분은 한 번만 낸다.
                    if (!emitted.insert(key).second)
                    {
                        continue;
                    }
                    m_TopologyViz.push_back({ from, to, color });
                }
            }
        }

        // 셀 방식이면 메시 간선 색칠 또는 셀 그래프 그리기를 선택해 선분을 만든다.
        if (method == 1)
        {
            if (drawGraph)
            {
                // 십자 크기는 셀 크기에 월드 배율을 곱해 정한다.
                const float worldScale = float(Math::Length(Math::Vector3(world.GetX())));
                AddCellGraphLines(m_TopologyViz, *graph, positions, cellSides, cellLabels, neighbors,
                    0.15f * model.m_LatticeTopology.CellSize * worldScale);
            }
            else
            {
                AddCellIslandWireframe(m_TopologyViz, model.m_LatticeTopology, positions, distances, vertexIsland);
            }
        }

        // 나눈 기준 평면을 흰색 테두리와 8x8 격자로 같은 snapshot에 덧붙인다.
        const Color planeColor(1.0f, 1.0f, 1.0f);
        const auto& corners = plane.WorldCorners;
        for (uint32_t e = 0; e < 4; ++e)
        {
            m_TopologyViz.push_back({ corners[e], corners[(e + 1) % 4], planeColor });
        }
        for (uint32_t g = 1; g < 8; ++g)
        {
            // 코너 (-,-)~(+,-)와 (-,+)~(+,+) 사이, (-,-)~(-,+)와 (+,-)~(+,+) 사이를 보간한다.
            const float t = float(g) / 8.0f;
            m_TopologyViz.push_back({ corners[0] + (corners[1] - corners[0]) * t,
                corners[3] + (corners[2] - corners[3]) * t, planeColor });
            m_TopologyViz.push_back({ corners[0] + (corners[3] - corners[0]) * t,
                corners[1] + (corners[2] - corners[1]) * t, planeColor });
        }

        // 평면 중심에서 법선 방향으로 짧은 화살표를 그린다.
        const Math::Vector3 center = (corners[0] + corners[2]) * 0.5f;
        const Math::Vector3 tip = center + normal * 0.15f;
        m_TopologyViz.push_back({ center, tip, planeColor });
        const Math::Vector3 side = Math::Normalize(corners[1] - corners[0]);
        m_TopologyViz.push_back({ tip, tip - normal * 0.04f + side * 0.025f, planeColor });
        m_TopologyViz.push_back({ tip, tip - normal * 0.04f - side * 0.025f, planeColor });

        // 아일랜드 수와 크기 순서별 side, 삼각형 수, 기여 상위 두 joint를 기록한다.
        PatchLog("TIV method=%u neighbors=%u draw=%u islands=%u triangles=%zu lines=%zu\n", method, neighbors,
            drawGraph ? 1u : 0u, method == 0 ? labels.IslandCount : cellLabels.IslandCount, triangles.size(),
            m_TopologyViz.size());

        // 셀 방식이면 아일랜드별 side, 셀 수, 정점 weight 기준 상위 두 joint를 기록한다.
        if (method == 1)
        {
            const auto jointScores = CellIslandJointScores(model.m_SkinCpuSections, vertexIsland, cellLabels.IslandCount);
            for (uint32_t i = 0; i < cellLabels.IslandCount; ++i)
            {
                // 점수 상위 두 joint를 고른다.
                std::vector<std::pair<uint32_t, double>> scores(jointScores[i].begin(), jointScores[i].end());
                const size_t top = std::min<size_t>(2, scores.size());
                std::partial_sort(scores.begin(), scores.begin() + top, scores.end(),
                    [](const auto& a, const auto& b) { return a.second > b.second; });

                // 한 줄로 적는다.
                std::ostringstream line;
                line << "TIV island=" << i << " color=" << i % 12 << " side=" << int(cellLabels.IslandSide[i])
                    << " cells=" << cellLabels.IslandCells[i];
                for (size_t k = 0; k < top; ++k)
                {
                    const uint32_t node = model.m_JointIndices[scores[k].first];
                    line << " joint" << k << "=" << SafeDiagnosticNodeName(target.GetName(), node)
                        << ":" << scores[k].second;
                }
                line << '\n';
                PatchLog("%s", line.str().c_str());
            }

            // 셀 방식 기록을 마쳤으므로 아래 나머지는 메시 방식 전용이다.
            return;
        }

        // 메시 방식이면 크기 순서별로 기록한다.
        for (uint32_t i = 0; i < order.size(); ++i)
        {
            // 점수 상위 두 joint를 고른다.
            const auto& island = labels.Islands[order[i]];
            std::vector<std::pair<uint32_t, double>> scores(island.JointScores.begin(), island.JointScores.end());
            const size_t top = std::min<size_t>(2, scores.size());
            std::partial_sort(scores.begin(), scores.begin() + top, scores.end(),
                [](const auto& a, const auto& b) { return a.second > b.second; });

            // 가능하면 node 이름으로 적고 없으면 번호가 대신 나온다.
            std::ostringstream line;
            line << "TIV island=" << i << " color=" << i % 12 << " side=" << island.Side
                << " tris=" << island.Triangles;
            for (size_t k = 0; k < top; ++k)
            {
                const uint32_t node = model.m_JointIndices[scores[k].first];
                line << " joint" << k << "=" << SafeDiagnosticNodeName(target.GetName(), node)
                    << ":" << scores[k].second;
            }
            line << '\n';
            PatchLog("%s", line.str().c_str());
        }
    }

    // 실제 추출 section의 공통 N과 그 node 식별자를 저장하고 팔레트 및 경계 weight를 확인한다.
    void SkeletalPatchCopy::CheckPatchSkin(const SkeletalPatchBuffers& patch)
    {
        // 추출 전역 삼각형 목록을 section 범위로 나누어 실제 걸친 N만 비교한다.
        if (m_SkinSnapshots.empty() || !m_SourceRevision) return;
        const std::set<uint32_t> triangles(patch.Triangles.begin(), patch.Triangles.end());
        uint32_t first = 0;
        bool found = false;
        bool allowed = true;
        uint32_t meshNode = UINT32_MAX;
        Math::Matrix4 common(Math::kIdentity);
        for (size_t s = 0; s < m_SkinSectionNodes.size(); ++s)
        {
            const auto& mesh = m_SourceRevision->GetMesh(s);
            uint32_t count = 0;
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
                count += mesh.draw[d].primCount / 3;
            const auto triangle = triangles.lower_bound(first);
            if (triangle != triangles.end() && *triangle < first + count)
            {
                // 공통 N의 첫 실제 추출 section을 매 프레임 재계산할 기준으로 보존한다.
                if (!found)
                {
                    common = m_SkinSectionNodes[s];
                    meshNode = mesh.meshCBV;
                }
                found = true;
                DirectX::XMFLOAT4X4 relative;
                DirectX::XMStoreFloat4x4(&relative, Math::Invert(common) * m_SkinSectionNodes[s]);
                for (uint32_t r = 0; r < 4; ++r)
                    for (uint32_t c = 0; c < 4; ++c)
                        allowed &= std::isfinite(relative.m[r][c])
                            && std::abs(relative.m[r][c] - (r == c ? 1.0f : 0.0f)) <= 1e-4f;
            }
            first += count;
        }

        // 정상 실행에서는 snapshot만 확정하며 체크의 Stale 시도는 로그에서 제외한다.
        for (uint32_t side = 0; side < m_SkinSnapshots.size(); ++side)
        {
            auto& snapshot = m_SkinSnapshots[side];
            snapshot.SkinningAllowed = found && allowed;
            snapshot.MeshNode = meshNode;
            if (found) snapshot.MeshNodeAtCut = common;
            if (!m_SkinCheckCase || m_ForceStale) continue;
            uint32_t frozen = 0;
            for (uint8_t value : snapshot.Frozen)
                frozen += value != 0;

            // Validate의 경계 정점마다 weight 합을 정규화한 뒤 고정 본의 최대 합을 구한다.
            float boundaryMax = 0;
            for (const auto& skin : patch.BoundarySkins)
            {
                uint32_t total = 0;
                uint32_t fixed = 0;
                for (uint32_t slot = 0; slot < 4; ++slot)
                {
                    total += skin.Weights[slot];
                    if (skin.Joints[slot] < snapshot.Frozen.size() && snapshot.Frozen[skin.Joints[slot]])
                        fixed += skin.Weights[slot];
                }
                if (total) boundaryMax = std::max(boundaryMax, float(fixed) / total);
            }

            // 경계가 없으면 N/A로 표시하고 PASS는 팔레트 오차만으로 판정한다.
            char weight[32] = "N/A";
            if (!patch.BoundarySkins.empty()) snprintf(weight, sizeof(weight), "%g", boundaryMax);
            PatchLog("PS1 case=%u side=%s attach=%u frozen=%u joints=%u skinningAllowed=%d paletteMaxError=%g boundaryFrozenWeightMax=%s result=%s\n",
                m_SkinCheckCase, m_SkinSnapshots.size() == 1 ? "D5" : (side ? "B" : "A"), snapshot.AttachNode,
                frozen, uint32_t(snapshot.JointsAtCut.size()), int(snapshot.SkinningAllowed), m_SkinPaletteErrors[side],
                weight, m_SkinPaletteErrors[side] <= 1e-5f ? "PASS" : "FAIL");
        }
    }

    // 임시 스키닝 PG를 절단 시점에 한 번 갱신하여 component-local Data와 비교한다.
    static void ValidatePieceSkinPose(const PatchSkinPublication* skin, const GeometryBuffers& geometry,
        uint32_t diagnosticCase, const char* label)
    {
        // 생성 실패와 빈 결과는 PASS로 집계하지 않는다.
        double maxError = INFINITY;
        uint32_t vertices = 0;
        if (skin && skin->VertexCount)
        {
            // 보관된 section 범위와 t_cut 팔레트로 진단 전용 PG를 준비한다.
            std::vector<PosedGeometry::VertexRange> sections;
            for (const auto& section : skin->Sections)
                sections.push_back({ section.FirstVertex, section.VertexCount });
            const auto& snapshot = skin->Snapshot;
            std::vector<Math::Matrix4> nodes(size_t(snapshot.AttachNode) + 1, Math::Matrix4(Math::kIdentity));
            nodes[snapshot.AttachNode] = snapshot.AttachAtCut;
            std::vector<Joint> palette;
            BuildPatchPalette(snapshot, snapshot.JointsAtCut.data(), nodes.data(), palette);
            PosedGeometry posed(skin->Vertices.Data, skin->Vertices.Stride, skin->Vertices.Flags, sections);

            // 실제 PreparePoseCS 출력을 읽고 VB 겸용 Data의 읽기 상태를 복원한다.
            ReadbackBuffer actualReadback;
            ReadbackBuffer expectedReadback;
            auto& context = GraphicsContext::Begin(L"PS3 skinned generated pose check");
            posed.UpdateSkinnedGenerated(context, skin->Vertices.Data, palette, snapshot.MeshNodeAtCut);
            auto& output = const_cast<ByteAddressBuffer&>(posed.GetBuffer());
            auto& reference = const_cast<ByteAddressBuffer&>(geometry.Data);
            actualReadback.Create(L"PS3 RAW40 readback", uint32_t(output.GetBufferSize()), 1);
            expectedReadback.Create(L"PS3 Data readback", uint32_t(reference.GetBufferSize()), 1);
            context.CopyBuffer(actualReadback, output);
            context.CopyBuffer(expectedReadback, reference);
            context.TransitionResource(output, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.TransitionResource(reference, D3D12_RESOURCE_STATE_GENERIC_READ);
            context.Finish(true);

            // section별 RAW40 offset과 원래 Data의 28바이트 정점 위치를 대응시킨다.
            const auto* actual = static_cast<const uint8_t*>(actualReadback.Map());
            const auto* expected = static_cast<const uint8_t*>(expectedReadback.Map());
            maxError = 0.0;
            for (uint32_t i = 0; i < sections.size(); ++i)
            {
                const auto& section = sections[i];
                for (uint32_t v = 0; v < section.VertexCount; ++v)
                {
                    float a[3];
                    float b[3];
                    std::memcpy(a, actual + posed.GetOffset(i) + size_t(v) * PosedGeometry::VertexStride, sizeof(a));
                    std::memcpy(b, expected + (size_t(section.FirstVertex) + v) * 28, sizeof(b));
                    const double dx = double(a[0]) - b[0];
                    const double dy = double(a[1]) - b[1];
                    const double dz = double(a[2]) - b[2];
                    const double error = std::sqrt(dx * dx + dy * dy + dz * dz) * 1000.0;
                    maxError = std::isfinite(error) ? std::max(maxError, error) : INFINITY;
                    ++vertices;
                }
            }

            // 동기 완료 뒤 매핑을 닫고 범위를 벗어나며 진단 PG를 폐기한다.
            actualReadback.Unmap();
            expectedReadback.Unmap();
        }

        // 지정된 mm 허용치와 로그 형식으로 case별 A/B 결과를 기록한다.
        PatchLog("PS3 case=%u side=%s vertices=%u maxError_mm=%.6f result=%s\n",
            diagnosticCase, label, vertices, maxError, maxError <= 0.01 ? "PASS" : "FAIL");
    }

    // A 조각 정점 중 SP7e2 경계 짝(위치 기준)과 경계 플래그를 대조해 누락(짝인데 플래그 없음)과 초과(플래그인데 짝 아님)를 센다.
    // 짝과 플래그는 모두 A 조각 skin VB 정점 번호로 비교하며, 복사 정점이 아닌 정점은 플래그가 없는 것으로 본다.
    void SkeletalPatchCopy::CheckBoundaryFlags(const MeshComponent& pieceA)
    {
        // 플래그나 A 조각 skin이 없으면 대조하지 않는다.
        if (m_PatchBoundaryFlags.empty() || !pieceA.PatchSkin || !pieceA.GetGeometry() || !pieceA.GetGeometry()->GetGenerated())
            return;

        // A 조각의 Provenance를 읽어 정점별 추출 정점 번호를 얻는다.
        const auto& skin = *pieceA.PatchSkin;
        auto& provenanceBuffer = const_cast<ByteAddressBuffer&>(pieceA.GetGeometry()->GetGenerated()->Provenance);
        ReadbackBuffer readback;
        readback.Create(L"LW2a provenance", uint32_t(provenanceBuffer.GetBufferSize()), 1);
        auto& context = GraphicsContext::Begin(L"LW2a provenance readback");
        context.CopyBuffer(readback, provenanceBuffer);
        context.TransitionResource(provenanceBuffer, D3D12_RESOURCE_STATE_GENERIC_READ);
        context.Finish(true);
        const auto* provenance = static_cast<const uint32_t*>(readback.Map());

        // SP7e2가 A 조각(piece 0)에서 경계로 짝지은 skin 정점 집합을 만든다.
        std::set<uint32_t> paired;
        for (const auto& pair : m_BoundaryPairs)
            if (pair.Piece == 0 && pair.PieceSection < skin.Sections.size())
                paired.insert(skin.Sections[pair.PieceSection].FirstVertex + pair.PieceVertex);

        // 짝이 있는데 플래그가 없는 정점(누락)과 플래그가 있는데 짝이 없는 정점(초과)을 센다.
        size_t flagged = 0, missed = 0, extra = 0;
        const auto flagOf = [&](uint32_t vertex)
        {
            const uint32_t* origin = provenance + size_t(vertex) * 8;
            return origin[0] != 0xffffffffu && origin[2] == origin[3] && origin[2] < m_PatchBoundaryFlags.size()
                && m_PatchBoundaryFlags[origin[2]];
        };
        for (const auto& section : skin.Sections)
            for (uint32_t v = section.FirstVertex; v < section.FirstVertex + section.VertexCount; ++v)
            {
                const bool flag = flagOf(v);
                flagged += flag;
                extra += flag && !paired.count(v);
            }
        for (uint32_t vertex : paired)
            missed += !flagOf(vertex);
        readback.Unmap();

        // 결과를 한 줄로 남긴다(누락은 0이어야 한다).
        PatchLog("LW2a vs SP7e2 flaggedPieceVertices=%zu sp7e2Paired=%zu missed=%zu extra=%zu %s\n",
            flagged, paired.size(), missed, extra, missed == 0 ? "PASS" : "FAIL");
    }

    // 생성 skin VB를 보관하고(남은 길이 옵션이 켜진 A 조각은 비경계 복사 정점 weight를 재분배한다) 진단 옵션에서 Data 비교와 rim/cap 대응 쌍 생성을 수행한다.
    void SkeletalPatchCopy::BuildPieceSkin(MeshComponent& piece, const GeometryBuffers& geometry,
        SkeletalPatchBuffers& patch, uint32_t body)
    {
        // 원본 revision과 출처는 동기 API가 반환할 때까지 호출자가 보유한다.
        const auto& snapshot = m_SkinSnapshots[body];
        if (!snapshot.SkinningAllowed || !m_SourceRevision)
            return;
        auto skin = std::make_shared<PatchSkinPublication>();
        skin->Snapshot = snapshot;
        skin->Side = m_SkinSnapshots.size() == 1 ? "D5" : (body ? "B" : "A");

        // 옵션이 켜진 A 조각이면 재분배 대상 mesh 번호(A의 section이 쓰는 SourceMesh)를 모은다.
        const RemainLengthTable* lengthTable = body < m_RemainLengthTables.size() && !m_RemainLengthTables[body].Ratio.empty()
            ? &m_RemainLengthTables[body] : nullptr;
        std::vector<uint32_t> lengthMeshes;
        if (lengthTable)
        {
            for (const auto& section : piece.GetGeometry()->GetSections())
            {
                lengthMeshes.push_back(section.SourceMesh);
            }
        }

        // 빌드가 검증을 통과한 뒤에만 경계 플래그를 계산한다(같은 절단에서 이미 있으면 재사용하고, 끝나면 기존 제거 상태 복사본을 놓는다).
        const std::function<const std::vector<uint32_t>*()> boundaryProvider = [&]() -> const std::vector<uint32_t>*
        {
            if (m_PatchBoundaryFlags.empty())
            {
                m_PatchBoundaryFlags = BuildPatchBoundaryFlags(*m_SourceRevision, patch.Triangles, patch.PreviousState, m_SkinSectionNodes);
                patch.PreviousState = {};
                const size_t flagged = size_t(std::count(m_PatchBoundaryFlags.begin(), m_PatchBoundaryFlags.end(), 1u));
                PatchLog("LW2a boundaryFlags patchVertices=%zu flagged=%zu\n", m_PatchBoundaryFlags.size(), flagged);
            }
            return &m_PatchBoundaryFlags;
        };

        // skin VB를 만들고, 재분배가 켜져 있으면 계수를 로그로 남긴다.
        const bool built = BuildPatchSkinVertices(*m_SourceRevision, patch.TriangleSources,
            geometry, snapshot, skin->Vertices, patch.Islands.TriangleSideBody,
            lengthTable, lengthTable ? &lengthMeshes : nullptr, lengthTable ? boundaryProvider : nullptr);
        if (built && lengthTable)
        {
            const auto& counters = skin->Vertices.LengthCounters;
            if (skin->Vertices.LengthEnabled)
            {
                PatchLog("LW2b reweighted=%u unchanged=%u boundaryKept=%u sumZeroFallback=%u fallbackMissingKept=%u singularKept=%u normalSingularKept=%u\n",
                    counters[0], counters[5], counters[1], counters[2], counters[3], counters[4], counters[6]);
                PatchLog("LW3 endpoint reweighted=%u unchanged=%u boundaryKept=%u sumZeroFallback=%u fallbackMissingKept=%u singularKept=%u normalSingularKept=%u normalChanged=%u\n",
                    counters[8], counters[13], counters[9], counters[10], counters[11], counters[12], counters[14], counters[15]);
            }
            else
            {
                PatchLog("LW2b disabled (flags empty or table size mismatch)\n");
            }
        }
        if (built)
        {
            for (const auto& section : piece.GetGeometry()->GetSections())
            {
                const auto& mesh = geometry.Meshes[section.SourceMesh];
                const uint32_t count = mesh.vbSize / 28;
                skin->Sections.push_back({ mesh.vbOffset / 28, count });
                skin->VertexCount += count;
            }
            piece.PatchSkin = skin;
        }

        // 전체 VB readback과 CPU 비교는 요청된 자동 진단에서만 수행한다.
        uint32_t checks = 0;
        CommandLineArgs::GetInteger(L"skeletal-patch-break-checks", checks);
        if (m_SkinSnapshots.size() == 1 && !checks)
            CommandLineArgs::GetInteger(L"rig-bone-cut-checks", checks);
        if (!checks || m_ForceStale)
            return;
        const char* label = m_SkinSnapshots.size() == 1 ? "D5" : (body ? "B" : "A");
        uint32_t vertices = 0;
        uint32_t copied = 0;
        uint32_t split = 0;
        uint32_t cap = 0;
        skin->Rims2 = skin->Caps3 = skin->FallbackK = 0;

        // 옵션이 켜진 A 조각이면 비경계 정점의 자손 가중치 합(그루터기 끌림의 원인)을 모은다.
        uint32_t stumpVertices = 0;
        double stumpSum = 0.0, stumpMax = 0.0;
        uint32_t copiedNonBoundary = 0, nonBoundaryDescendant = 0, copiedBoundary = 0;

        // 복사 정점의 게시 순간 법선(새 레코드 스키닝)과 절단 시점 Data 법선의 최대 각도(도)와 최대 성분 차를 모은다.
        uint32_t normalChecked = 0;
        double normalMaxAngle = 0.0, normalMaxComponent = 0.0;

        // 끝점 보간 정점(1 내부 교점, 2 rim, 3 cap)의 수와 위치 오차 최댓값(mm), 재분배 없는 기준 빌드의 위치 오차와 기준 대비 위치 차 최댓값(mm), cap 끝점 절단 시점 법선 칸의 비교 수와 비트 차 수를 모은다.
        uint32_t endpointVertices[4] = {};
        double endpointMaxError[4] = {};
        double endpointBaselineDelta[4] = {};
        double endpointBaselineError[4] = {};
        uint32_t capNormalCount = 0;
        uint32_t capNormalDiff = 0;
        double capNormalMaxComponent = 0.0;
        PatchSkinVertices baseline;
        bool baselineBuilt = false;
        const bool follow = built && PatchAnimFollowEnabled() && snapshot.AnimFollow;
        double maxError = built ? 0.0 : INFINITY;
        if (built)
        {
            // VB 겸용 버퍼는 readback 뒤에도 GENERIC_READ로 복원한다.
            ReadbackBuffer readbacks[3];
            GpuBuffer* buffers[] = { &skin->Vertices.Data, &const_cast<ByteAddressBuffer&>(geometry.Data),
                &const_cast<ByteAddressBuffer&>(geometry.Provenance) };
            auto& context = GraphicsContext::Begin(L"PS2 skin readback");
            for (uint32_t i = 0; i < 3; ++i)
            {
                readbacks[i].Create(L"PS2 readback", buffers[i]->GetBufferSize(), 1);
                context.CopyBuffer(readbacks[i], *buffers[i]);
                context.TransitionResource(*buffers[i], D3D12_RESOURCE_STATE_GENERIC_READ);
            }
            context.Finish(true);
            // 재분배 없는 기준 skin VB를 같은 입력으로 한 번 더 만들어 읽는다(옵션이 켜진 A 조각의 끝점 위치와 cap 절단 시점 법선 칸 비교용이다).
            ReadbackBuffer baselineReadback;
            const bool hasBaseline = lengthTable
                && BuildPatchSkinVertices(*m_SourceRevision, patch.TriangleSources, geometry, snapshot, baseline, patch.Islands.TriangleSideBody)
                && baseline.Stride == skin->Vertices.Stride && baseline.Data.GetBufferSize() == skin->Vertices.Data.GetBufferSize();
            baselineBuilt = hasBaseline;
            if (hasBaseline)
            {
                auto& baselineContext = GraphicsContext::Begin(L"LW3 baseline readback");
                baselineReadback.Create(L"LW3 baseline readback", baseline.Data.GetBufferSize(), 1);
                baselineContext.CopyBuffer(baselineReadback, baseline.Data);
                baselineContext.TransitionResource(baseline.Data, D3D12_RESOURCE_STATE_GENERIC_READ);
                baselineContext.Finish(true);
            }

            // 현재 pose 대신 실제 t_cut joint와 부착 node를 전달하여 고정 본과 K를 복원한다.
            std::vector<Math::Matrix4> nodes(size_t(snapshot.AttachNode) + 1, Math::Matrix4(Math::kIdentity));
            nodes[snapshot.AttachNode] = snapshot.AttachAtCut;
            std::vector<Joint> palette;
            BuildPatchPalette(snapshot, snapshot.JointsAtCut.data(), nodes.data(), palette);
            const auto* bind = static_cast<const uint8_t*>(readbacks[0].Map());
            const auto* data = static_cast<const uint8_t*>(readbacks[1].Map());
            const auto* provenance = static_cast<const uint32_t*>(readbacks[2].Map());

            // 기준 skin VB의 끝점 보간 위치를 같은 방식(두 끝점을 각각 skin한 뒤 t로 보간)으로 구하는 함수이다.
            const auto* baselineBytes = hasBaseline ? static_cast<const uint8_t*>(baselineReadback.Map()) : nullptr;
            const auto interpolatedPosition = [&](const uint8_t* record)
            {
                float t;
                std::memcpy(&t, record + skin->Vertices.Stride - kSkinExtT, sizeof(t));
                Math::Vector4 ends[2];
                for (uint32_t e = 0; e < 2; ++e)
                {
                    float endBind[3];
                    uint16_t ids[4], ws[4];
                    const auto* endpoint = record + skin->Vertices.Stride - kSkinExtEndpoint0 + e * kSkinEndpointSize;
                    std::memcpy(endBind, endpoint, sizeof(endBind));
                    std::memcpy(ids, endpoint + kSkinEndpointJoints, sizeof(ids));
                    std::memcpy(ws, endpoint + kSkinEndpointWeights, sizeof(ws));
                    const float total = float(std::max(1u, uint32_t(ws[0]) + ws[1] + ws[2] + ws[3]));
                    ends[e] = Math::Vector4(Math::kZero);
                    for (uint32_t k = 0; k < 4 && ids[k] < palette.size(); ++k)
                        ends[e] = ends[e] + (palette[ids[k]].posXform * Math::Vector4(endBind[0], endBind[1], endBind[2], 1)) * (float(ws[k]) / total);
                }
                return ends[0] * (1 - t) + ends[1] * t;
            };
            std::vector<uint32_t> rimVertices;
            std::vector<uint32_t> capVertices;
            for (const auto& section : skin->Sections)
            {
                for (uint32_t v = section.FirstVertex; v < section.FirstVertex + section.VertexCount; ++v)
                {
                    // stride 말미의 u16 joint와 weight를 PreparePoseCS와 같은 합으로 정규화한다.
                    float position[3];
                    float expected[3];
                    uint16_t joints[4];
                    uint16_t weights[4];
                    const auto* input = bind + size_t(v) * skin->Vertices.Stride;
                    std::memcpy(position, input, sizeof(position));
                    std::memcpy(expected, data + size_t(v) * 28, sizeof(expected));
                    std::memcpy(joints, input + skin->Vertices.Stride - 16, sizeof(joints));
                    std::memcpy(weights, input + skin->Vertices.Stride - 8, sizeof(weights));
                    const float total = float(std::max(1u, uint32_t(weights[0]) + weights[1] + weights[2] + weights[3]));
                    const Math::Vector4 source(position[0], position[1], position[2], 1.0f);
                    Math::Vector4 posed(Math::kZero);
                    bool valid = true;
                    for (uint32_t k = 0; k < 4; ++k)
                    {
                        if (joints[k] >= palette.size())
                        {
                            valid = false;
                            continue;
                        }
                        posed = posed + (palette[joints[k]].posXform * source) * (float(weights[k]) / total);
                    }

                    // 비율 0인 자손 joint에 걸린 가중치 합을 구해 평균과 최댓값에 반영한다.
                    if (lengthTable)
                    {
                        float descendant = 0.0f;
                        for (uint32_t k = 0; k < 4; ++k)
                            if (joints[k] < lengthTable->Descendant.size() && lengthTable->Descendant[joints[k]])
                                descendant += float(weights[k]) / total;
                        if (descendant > 0.0f)
                        {
                            ++stumpVertices;
                            stumpSum += descendant;
                            stumpMax = std::max(stumpMax, double(descendant));
                        }

                        // 복사 정점 중 경계가 아닌 정점에 자손 가중치가 남았는지 센다(재분배 뒤 0이어야 한다).
                        const auto* flagOrigin = provenance + size_t(v) * 8;
                        const bool copiedVertex = flagOrigin[0] != UINT32_MAX && flagOrigin[2] == flagOrigin[3];
                        const bool boundary = flagOrigin[2] >= m_PatchBoundaryFlags.size() || m_PatchBoundaryFlags[flagOrigin[2]];
                        if (copiedVertex && !boundary)
                        {
                            ++copiedNonBoundary;
                            nonBoundaryDescendant += descendant > 0.0f;
                        }
                        copiedBoundary += copiedVertex && boundary;

                        // 복사 정점은 새 레코드의 법선을 절단 시점 팔레트로 스키닝해 Data 법선과 비교한다(Data는 노드 변환을 포함하므로 역전치를 곱한다).
                        if (copiedVertex && valid)
                        {
                            const auto decode = [](uint32_t packed)
                            {
                                return Math::Vector3(float(packed & 1023) / 1023.0f * 2 - 1,
                                    float((packed >> 10) & 1023) / 1023.0f * 2 - 1, float((packed >> 20) & 1023) / 1023.0f * 2 - 1);
                            };
                            uint32_t recordNormal, dataNormal;
                            std::memcpy(&recordNormal, input + 12, 4);
                            std::memcpy(&dataNormal, data + size_t(v) * 28 + 12, 4);
                            Math::Vector3 skinned(Math::kZero);
                            for (uint32_t k = 0; k < 4; ++k)
                                skinned = skinned + (palette[joints[k]].nrmXform * decode(recordNormal)) * (float(weights[k]) / total);
                            const Math::Vector3 actualNormal = Math::Normalize(
                                Math::InverseTranspose(snapshot.MeshNodeAtCut.Get3x3()) * skinned);
                            const Math::Vector3 expectedNormal = Math::Normalize(decode(dataNormal));
                            const double cosine = std::clamp(double(float(Math::Dot(actualNormal, expectedNormal))), -1.0, 1.0);
                            normalMaxAngle = std::max(normalMaxAngle, std::acos(cosine) * 57.29577951308232);
                            const Math::Vector3 difference = actualNormal - expectedNormal;
                            normalMaxComponent = std::max(normalMaxComponent, double(std::max({ std::fabs(float(difference.GetX())),
                                std::fabs(float(difference.GetY())), std::fabs(float(difference.GetZ())) })));
                            ++normalChecked;
                        }
                    }

                    // 끝점 보간 종류(1 내부 교점, 2 rim, 3 cap)가 있으면 GPU와 같이 두 원본 끝점을 각각 skin한 뒤 t로 보간한다.
                    uint32_t interpolation = 0;
                    if (skin->Vertices.Flags & kSkinExtFlag)
                        std::memcpy(&interpolation, input + skin->Vertices.Stride - kSkinExtInternal, sizeof(interpolation));
                    if (interpolation)
                    {
                        Math::Vector4 endpoints[2];
                        float t;
                        std::memcpy(&t, input + skin->Vertices.Stride - kSkinExtT, 4);
                        for (uint32_t e = 0; e < 2; ++e)
                        {
                            const auto* endpoint = input + skin->Vertices.Stride - kSkinExtEndpoint0 + e * kSkinEndpointSize;
                            std::memcpy(position, endpoint, sizeof(position));
                            std::memcpy(joints, endpoint + kSkinEndpointJoints, sizeof(joints));
                            std::memcpy(weights, endpoint + kSkinEndpointWeights, sizeof(weights));
                            const float sum = float(std::max(1u, uint32_t(weights[0]) + weights[1] + weights[2] + weights[3]));
                            endpoints[e] = Math::Vector4(Math::kZero);
                            for (uint32_t k = 0; k < 4; ++k)
                            {
                                if (joints[k] >= palette.size())
                                {
                                    valid = false;
                                    continue;
                                }
                                endpoints[e] = endpoints[e] + (palette[joints[k]].posXform
                                    * Math::Vector4(position[0], position[1], position[2], 1)) * (float(weights[k]) / sum);
                            }
                        }
                        posed = endpoints[0] * (1 - t) + endpoints[1] * t;
                    }

                    // N(t_cut) 적용 뒤 미터를 mm로 바꾸며 비유한 값과 잘못된 joint는 FAIL로 남긴다.
                    const Math::Vector3 actual(snapshot.MeshNodeAtCut * posed);
                    const double error = double(float(Math::Length(actual
                        - Math::Vector3(expected[0], expected[1], expected[2])))) * 1000.0;
                    maxError = !valid || !std::isfinite(error) ? INFINITY : std::max(maxError, error);

                    // 끝점 보간 정점의 위치 오차와 기준 빌드 대비 위치 차를 모은다.
                    if (interpolation && interpolation < 4)
                    {
                        ++endpointVertices[interpolation];
                        endpointMaxError[interpolation] = !valid || !std::isfinite(error) ? INFINITY : std::max(endpointMaxError[interpolation], error);
                    }
                    if (interpolation && interpolation < 4 && baselineBytes)
                    {
                        const auto* baselineRecord = baselineBytes + size_t(v) * skin->Vertices.Stride;
                        const Math::Vector3 moved(snapshot.MeshNodeAtCut * (posed - interpolatedPosition(baselineRecord)));
                        const double delta = double(float(Math::Length(moved))) * 1000.0;
                        endpointBaselineDelta[interpolation] = std::isfinite(delta) ? std::max(endpointBaselineDelta[interpolation], delta) : INFINITY;
                        const Math::Vector3 baselineActual(snapshot.MeshNodeAtCut * interpolatedPosition(baselineRecord));
                        const double baselineError = double(float(Math::Length(baselineActual - Math::Vector3(expected[0], expected[1], expected[2])))) * 1000.0;
                        endpointBaselineError[interpolation] = std::isfinite(baselineError) ? std::max(endpointBaselineError[interpolation], baselineError) : INFINITY;
                    }
                    if (interpolation == 3 && baselineBytes)
                    {
                        const auto* baselineRecord = baselineBytes + size_t(v) * skin->Vertices.Stride;
                        for (uint32_t e = 0; e < 2; ++e)
                        {
                            const size_t slot = skin->Vertices.Stride - kSkinExtEndpoint0 + e * kSkinEndpointSize + kSkinEndpointTangent;
                            capNormalDiff += std::memcmp(input + slot, baselineRecord + slot, sizeof(uint32_t)) != 0;

                            // cap 방향은 저장된 끝점 skin으로 복원해 비교한다. 재분배하면 bind 방향 비트는 달라도 절단 시점 방향은 같아야 한다.
                            const auto cutDirection = [&](const uint8_t* record)
                            {
                                const auto* endpoint = record + skin->Vertices.Stride - kSkinExtEndpoint0 + e * kSkinEndpointSize;
                                uint32_t packed;
                                uint16_t endpointJoints[4], endpointWeights[4];
                                std::memcpy(&packed, endpoint + kSkinEndpointTangent, sizeof(packed));
                                std::memcpy(endpointJoints, endpoint + kSkinEndpointJoints, sizeof(endpointJoints));
                                std::memcpy(endpointWeights, endpoint + kSkinEndpointWeights, sizeof(endpointWeights));
                                const Math::Vector3 direction(float(packed & 1023) / 1023.0f * 2 - 1,
                                    float((packed >> 10) & 1023) / 1023.0f * 2 - 1, float((packed >> 20) & 1023) / 1023.0f * 2 - 1);

                                // 절단 시점 팔레트로 스키닝하고 크기를 제거한다(공통 node 변환 전 모델 공간).
                                Math::Vector3 skinned(Math::kZero);
                                for (uint32_t k = 0; k < 4; ++k)
                                {
                                    if (endpointJoints[k] >= palette.size())
                                        return Math::Vector3(INFINITY, INFINITY, INFINITY);
                                    skinned = skinned + (palette[endpointJoints[k]].nrmXform * direction) * float(endpointWeights[k]);
                                }
                                return Math::Normalize(skinned);
                            };
                            const Math::Vector3 difference = cutDirection(input) - cutDirection(baselineRecord);
                            const double component = std::max({ std::fabs(float(difference.GetX())),
                                std::fabs(float(difference.GetY())), std::fabs(float(difference.GetZ())) });
                            capNormalMaxComponent = std::isfinite(component) ? std::max(capNormalMaxComponent, component) : INFINITY;
                            ++capNormalCount;
                        }
                    }
                    const auto* origin = provenance + size_t(v) * 8;
                    if (origin[0] == UINT32_MAX)
                    {
                        ++cap;
                        capVertices.push_back(v);
                        if (interpolation == 3)
                            ++skin->Caps3;
                        else if (follow)
                            ++skin->FallbackK;
                    }
                    else if (origin[2] == origin[3])
                        ++copied;
                    else
                    {
                        ++split;

                        // 실제 경계(K 또는 끝점 보간 rim)만 cap과 짝지으며 내부 교점(1)은 표면 수에만 포함한다.
                        if (interpolation == 0 || interpolation == 2)
                            rimVertices.push_back(v);
                        if (interpolation == 2)
                            ++skin->Rims2;
                        else if (interpolation == 0 && follow)
                            ++skin->FallbackK;
                    }
                    ++vertices;
                }
            }

            // break 진단에서만 각 split을 t_cut Data에서 가장 가까운 cap과 대응시키고 그 거리를 기억한다.
            // cutter의 cap 링은 테두리와 수십 μm 다를 수 있으므로, 진단은 t_cut 대비 간격 변화를 본다.
            uint32_t seamChecks = 0;
            CommandLineArgs::GetInteger(L"skeletal-patch-break-checks", seamChecks);
            if (seamChecks)
            {
                uint32_t unmatched = 0;
                float nearestMax = 0.0f;
                for (uint32_t rim : rimVertices)
                {
                    uint32_t match = UINT32_MAX;
                    float nearest = INFINITY;
                    float r[3];
                    std::memcpy(r, data + size_t(rim) * 28, sizeof(r));
                    for (uint32_t candidate : capVertices)
                    {
                        float c[3];
                        std::memcpy(c, data + size_t(candidate) * 28, sizeof(c));
                        const float distance = float(Math::Length(Math::Vector3(r[0], r[1], r[2]) - Math::Vector3(c[0], c[1], c[2])));
                        if (distance < nearest)
                        {
                            nearest = distance;
                            match = candidate;
                        }
                    }

                    // 0.1 mm 안에 cap이 없는 split은 누락으로 남겨 진단에서 FAIL이 되게 한다.
                    if (nearest > 1e-4f)
                    {
                        match = UINT32_MAX;
                        ++unmatched;
                    }
                    else
                    {
                        nearestMax = std::max(nearestMax, nearest);
                    }
                    const float* bindPosition = reinterpret_cast<const float*>(bind + size_t(rim) * skin->Vertices.Stride);
                    skin->RimCapPairs.push_back({ rim, match, nearest, { bindPosition[0], bindPosition[1], bindPosition[2] } });
                }

                // split·cap 수, 누락 수, t_cut 최근접 거리 최댓값을 원인 조사용으로 남긴다.
                PatchLog("PS4 seam pairs rim=%zu cap=%zu unmatched=%u nearestAtCutMax_mm=%.6f\n",
                    rimVertices.size(), capVertices.size(), unmatched, double(nearestMax) * 1000.0);
            }

            // 동기 진단에서 사용한 버퍼(기준 빌드 포함)의 매핑을 해제한다.
            for (auto& readback : readbacks)
                readback.Unmap();
            if (baselineBytes)
                baselineReadback.Unmap();
        }

        // 생성 실패와 빈 결과도 오차 한계로 PASS가 되지 않게 같은 형식으로 기록한다.
        if (!vertices)
            maxError = INFINITY;
        PatchLog("PS2 case=%u side=%s vertices=%u copied=%u split=%u cap=%u maxError_mm=%.6f result=%s\n",
            m_SkinCheckCase, label, vertices, copied, split, cap, maxError, maxError <= 0.01 ? "PASS" : "FAIL");

        // 자손 가중치가 남은 A 조각 정점 수와 그 합의 평균·최댓값을 기록한다(주 레코드만 집계하며 끝점·경계 구분은 이후 단계).
        if (lengthTable)
            PatchLog("LW1 case=%u side=%s mainRecordOnly descendantWeightVertices=%u of %u avg=%.4f max=%.4f\n", m_SkinCheckCase, label,
                stumpVertices, vertices, stumpVertices ? stumpSum / stumpVertices : 0.0, stumpMax);
        if (lengthTable)
        {
            // 재분배가 꺼졌으면 FAIL, 켜졌으면 비경계 자손 가중치 0과 계수 합 = A 복사 정점 수, 경계 수 = LW-2a 경계 수를 확인한다.
            const auto& counters = skin->Vertices.LengthCounters;
            const bool enabled = skin->Vertices.LengthEnabled;
            const uint32_t accounted = counters[0] + counters[5] + counters[1] + counters[3] + counters[4];
            const bool countsMatch = enabled && accounted == copied && counters[1] == copiedBoundary;
            PatchLog("LW2b case=%u side=%s enabled=%d nonBoundaryCopied=%u nonBoundaryDescendantWeight=%u %s\n", m_SkinCheckCase, label,
                enabled ? 1 : 0, copiedNonBoundary, nonBoundaryDescendant, enabled && nonBoundaryDescendant == 0 ? "PASS" : "FAIL");
            PatchLog("LW2b case=%u side=%s counterSum=%u copied=%u boundaryKept=%u lw2aBoundary=%u %s\n", m_SkinCheckCase, label,
                accounted, copied, counters[1], copiedBoundary, countsMatch ? "PASS" : "FAIL");

            // 게시 순간 법선 차는 UNORM 10비트 반올림 두 번(레코드 재압축, Data 압축; 각 약 1e-3)의 합인 3e-3 안이어야 하고, 재분배된 법선이 있으면 일부는 원래 비트와 달라야 한다.
            const bool normalUpdated = counters[0] == 0 || counters[7] > 0;
            PatchLog("LW2c case=%u side=%s normalChecked=%u normalChanged=%u maxAngle_deg=%.4f maxComponent=%.6f %s%s\n", m_SkinCheckCase, label,
                normalChecked, counters[7], normalMaxAngle, normalMaxComponent,
                normalChecked && normalMaxComponent <= 3e-3 && normalUpdated ? "PASS" : "FAIL", normalUpdated ? "" : " normal not updated");

            // 끝점 레코드 계수의 합은 끝점 보간 정점 수의 두 배와 같아야 한다.
            const uint32_t endpointAccounted = counters[8] + counters[13] + counters[9] + counters[11] + counters[12];
            const uint32_t endpointRecords = 2 * (endpointVertices[1] + endpointVertices[2] + endpointVertices[3]);
            PatchLog("LW3 case=%u side=%s endpointRecords=%u counterSum=%u %s\n", m_SkinCheckCase, label,
                endpointRecords, endpointAccounted, enabled && endpointAccounted == endpointRecords ? "PASS" : "FAIL");

            // 끝점 위치 오차 증가는 1e-4 mm 이하, cap 복원 방향 차는 10비트 재압축 오차 범위인 3e-3 이하인지 확인한다.
            const double increaseMax = std::max({ endpointMaxError[1] - endpointBaselineError[1], endpointMaxError[2] - endpointBaselineError[2],
                endpointMaxError[3] - endpointBaselineError[3] });
            PatchLog("LW3 case=%u side=%s endpointPos_mm rim=%.6f(base %.6f, n=%u) cap=%.6f(base %.6f, n=%u) internal=%.6f(base %.6f, n=%u) increaseMax=%.6f deltaVsBaseline rim=%.6f cap=%.6f %s\n",
                m_SkinCheckCase, label, endpointMaxError[2], endpointBaselineError[2], endpointVertices[2],
                endpointMaxError[3], endpointBaselineError[3], endpointVertices[3], endpointMaxError[1], endpointBaselineError[1], endpointVertices[1],
                increaseMax, endpointBaselineDelta[2], endpointBaselineDelta[3], enabled && baselineBuilt && increaseMax <= 1e-4 ? "PASS" : "FAIL");
            PatchLog("LW3 case=%u side=%s capCutTimeNormals=%u bitDiff=%u maxComponent=%.6f %s\n", m_SkinCheckCase, label,
                capNormalCount, capNormalDiff, capNormalMaxComponent,
                enabled && baselineBuilt && capNormalMaxComponent <= 3e-3 ? "PASS" : "FAIL");
        }

        // PS3 동기 GPU 진단은 break 검사에서만 수행하며 게시 경로에는 연결하지 않는다.
        uint32_t poseChecks = 0;
        CommandLineArgs::GetInteger(L"skeletal-patch-break-checks", poseChecks);
        if (poseChecks)
            ValidatePieceSkinPose(built ? piece.PatchSkin.get() : nullptr, geometry, m_SkinCheckCase, label);
    }

    // 다음 Static 후보 조회 전에 작업 무효화와 owner 삭제를 완료한다.
    void SkeletalPatchCopy::Reset(Scene& scene, PhysicsSystem& physics, CuttingSystem& cutting)
    {
        // 추출 완료를 기다려 임시 자원을 해제하고 미게시 배치 snapshot도 버린다.
        m_Job.Cancel();
        m_ReadyCut.reset();
        DiscardBodyCompaction();
        m_Name.clear();
        m_SkinPatch.reset();
        m_SkinSnapshots.clear();
        m_PendingDetach = {};
        m_RemainLengthTables.clear();
        m_PatchBoundaryFlags.clear();
        m_Attachments.clear();
        m_PieceIds.clear();
        m_BoundaryWait = 0;
        m_SkinPaletteErrors.clear();
        m_BodySides = { 0, 1 };
        m_SkinSectionNodes.clear();
        m_BonePending = false;
        m_BonePublication = {};

        // Renderer 종료 전에 원본 모델 참조를 끊어 텍스처 참조가 관리자보다 오래 남지 않게 한다.
        m_SourceRevision.reset();

        // 준비물을 버릴 때 아직 실행하지 않은 break도 취소한다.
        physics.CancelBreak(m_RequestId);
        m_RequestId = 0;
        m_MultiBreaks.clear();
        m_MultiPieces.clear();
        m_BreakReady = false;

        // 사용자 owner 삭제와 같은 절단 취소·물리 매핑 제거 경로를 사용한다.
        if (m_CutInFlight) m_RetiredOwners.push_back(m_CopyOwner);
        m_CutPending = m_CutInFlight = m_Completed = false;
        if (scene.FindObject(m_CopyOwner)
            && !DeleteEditTarget(scene, &physics, &cutting, EditTarget::MakeOwner(m_CopyOwner)))
            throw std::runtime_error("Skeletal patch owner deletion failed");
        m_CopyOwner = 0;

        // 요청 취소와 별개로 살아 있는 owner의 게시 이력은 보존한다.
        PruneDetachHistory(scene);
    }

    // 삭제된 owner나 만료된 모델의 이력을 요청 취소와 별개로 회수한다.
    void SkeletalPatchCopy::PruneDetachHistory(const Scene& scene)
    {
        // 모델이 다른 owner에 공유되어 살아 있어도 삭제된 owner의 기록은 제거한다.
        m_DetachHistory.erase(std::remove_if(m_DetachHistory.begin(), m_DetachHistory.end(),
            [&scene](const DetachRecord& record)
            {
                // Scene의 객체 수명과 weak 모델 수명을 함께 검사한다.
                return !scene.FindObject(record.Owner) || record.SourceModel.expired();
            }), m_DetachHistory.end());
    }

    // 성공한 단일·다중 게시에서 이전 이력을 새 조각에 적용하고 이번 root별 이력을 추가한다.
    void SkeletalPatchCopy::PublishDetachRecord(const Scene& scene, GameObject& owner, const MeshComponent& source,
        const MeshComponent* newA, const std::vector<ComponentId>& newPieces, uint64_t request)
    {
        // 게시 유효성 판정 전에 만료된 이력을 회수한다.
        PruneDetachHistory(scene);

        // owner와 모델이 같은 요청만 소비하며 실패·취소 입력은 이력에 넣지 않는다.
        const auto model = owner.GetModelInstance()->GetModel();
        if (m_PendingDetach.Owner != owner.GetId() || m_PendingDetach.SourceModel.lock() != model
            || m_PendingDetach.RootNode == UINT32_MAX)
            return;

        // 새 A/B는 이번 절단 이전 이력만 따라잡고 기존 Frozen 값은 보존한다.
        for (const auto& record : m_DetachHistory)
            ApplyDetachRecord(owner, source, nullptr, record, request, &newPieces);

        // 모든 root는 같은 요청 시점 입력을 쓰며 성공 게시 뒤에만 이력에 들어간다.
        for (uint32_t root : m_PendingDetachRoots)
        {
            auto record = m_PendingDetach;
            record.RootNode = root;
            m_DetachHistory.push_back(std::move(record));
            if (!CutPerf::Quiet())
                PatchLog("SKINSTALE record owner=%llu cut=%llu root=%u\n", owner.GetId(), request, root);
            ApplyDetachRecord(owner, source, newA, m_DetachHistory.back(), request);
        }

        // 소비한 요청 입력을 비워 중복 게시에서 기록을 다시 추가하지 않는다.
        m_PendingDetach = {};
        m_PendingDetachRoots.clear();
    }

    // RS 설치 때 요청 당시 부모 snapshot에 owner의 최신 이력을 복사 후 교체한다.
    void SkeletalPatchCopy::ApplyRecutDetachHistory(Scene& scene, GameObject& owner, MeshComponent& child,
        const SkeletalRigState& rig)
    {
        // 해당 자식에만 전체 이력을 적용하며 다른 owner와 원본 publication은 보존한다.
        const std::vector<ComponentId> targets{ child.GetId() };
        for (const auto& record : m_DetachHistory)
            ApplyDetachRecord(owner, child, nullptr, record, 0, &targets);

        // 자식 CPU 사본과 현재 팔레트 유한성을 기존 진단으로 확인한다.
        if (!CutPerf::Quiet())
        {
            // 설치 직후 팔레트를 준비한 뒤 단일·다중 게시와 같은 시점에서 검사한다.
            auto& context = GraphicsContext::Begin(L"Diagnose recut detach history");
            scene.PrepareMeshes(context);
            context.Finish();
            PatchLog("SKINSTALE boundary=RS child=%llu owner=%llu\n", child.GetId(), owner.GetId());
        }
        LogStaleSkin(owner, rig, 0);
    }

    // 한 분리 기록을 대상 패치에 멱등 적용하고 기존 정점 경계 진단을 재사용한다.
    void SkeletalPatchCopy::ApplyDetachRecord(GameObject& owner, const MeshComponent& source,
        const MeshComponent* newA, const DetachRecord& record, uint64_t request,
        const std::vector<ComponentId>* targets)
    {
        // owner와 원본 모델이 같은 이력만 적용한다.
        const auto model = owner.GetModelInstance()->GetModel();
        if (record.Owner != owner.GetId() || record.SourceModel.lock() != model)
            return;
        const auto& parents = owner.GetModelInstance()->GetPoseParents();

        // NodeId의 부모 체인으로 이번 분리 subtree 소속을 판정한다.
        const auto inside = [&parents, &record](uint32_t node)
        {
            // 잘못된 노드는 subtree에 포함하지 않는다.
            for (size_t depth = 0; node < parents.size() && depth < parents.size(); ++depth)
            {
                if (node == record.RootNode)
                    return true;
                node = parents[node];
            }
            return false;
        };

        // 새 A 팔레트는 게시 프레임 입력으로 한 번 만들어 슬롯 진단의 기준으로 쓴다.
        std::vector<Joint> aPalette;
        if (!CutPerf::Quiet() && newA && newA->HasSkinnedPatch())
            BuildPatchPalette(newA->PatchSkin->Snapshot, source.GetPreparedJoints(), source.GetPreparedNodes(), aPalette,
                source.GetPreparedAnimJoints(), source.GetPreparedAnimNodes(), PatchAnimFollowEnabled());

        // 활성 publication만 복사하여 RS가 참조 중인 이전 snapshot을 보존한다.
        for (const auto& component : owner.GetMeshComponents())
        {
            if ((targets && std::find(targets->begin(), targets->end(), component->GetId()) == targets->end())
                || !component->HasSkinnedPatch() || component->GetGeometry()->GetSource() != model
                || component->PatchSkin->Snapshot.SourceFrozen || inside(component->PatchSkin->Snapshot.AttachNode))
                continue;
            auto snapshot = component->PatchSkin->Snapshot;
            const size_t count = snapshot.JointsAtCut.size();
            const Math::Matrix4 inverseDelta = Math::Invert(record.Nodes[snapshot.AttachNode] * Math::Invert(snapshot.AttachAtCut));
            Math::Matrix4 correction(Math::kIdentity);
            if (snapshot.AnimFollow)
                correction = record.Nodes[snapshot.AttachNode] * Math::Invert(record.AnimNodes[snapshot.AttachNode]);
            std::vector<size_t> added;

            // K는 배열 밖에 있으므로 모델 팔레트 슬롯만 판정하고 기존 고정 값은 보존한다.
            for (size_t k = 0; k < count; ++k)
            {
                if (snapshot.Frozen[k] || !inside(model->m_JointIndices[k]))
                    continue;
                snapshot.Frozen[k] = 1;
                snapshot.JointsAtCut[k].posXform = inverseDelta * record.Palette[k].posXform;
                snapshot.JointsAtCut[k].nrmXform = Math::InverseTranspose(snapshot.JointsAtCut[k].posXform.Get3x3());
                if (snapshot.AnimFollow)
                    snapshot.AnimOffset[k] = Math::Invert(correction * record.AnimPalette[k].posXform) * record.Palette[k].posXform;
                if (snapshot.LengthBlendRatio.empty())
                    snapshot.LengthBlendRatio.assign(count, 1.0f);
                snapshot.LengthBlendRatio[k] = record.LengthRatio[k];
                added.push_back(k);
            }
            if (added.empty())
                continue;

            // ByteAddressBuffer의 ComPtr 복사는 기존 VB를 공유하며 GPU 버퍼를 생성하지 않는다.
            auto publication = std::make_shared<PatchSkinPublication>(*component->PatchSkin);
            publication->Snapshot = std::move(snapshot);
            component->PatchSkin = std::move(publication);

            // 이전 이력과 RS 적용은 root·슬롯 수를 남기고 기준 A가 있을 때만 경계를 비교한다.
            if (!CutPerf::Quiet())
                PatchLog("SKINSTALE apply patch=%llu cut=%llu root=%u newFrozen=%zu boundary=%s\n",
                    component->GetId(), request, record.RootNode, added.size(), newA ? "compare" : "history");

            // 실제 위치 팔레트로 신규 고정 슬롯만 교체했을 때의 정점 위치 차이를 기록한다.
            if (!CutPerf::Quiet() && newA)
            {
                const auto& skin = *component->PatchSkin;
                const auto& cpu = skin.Vertices.DiagnosticSkin;
                const size_t stride = skin.Vertices.Stride;
                if (aPalette.size() < count || cpu.empty() || stride < ((skin.Vertices.Flags & kSkinExtFlag) ? kSkinExtInternal : 28u)
                    || cpu.size() % stride != 0)
                {
                    PatchLog("SKINSTALE patch=%llu cut=%llu boundary=unavailable\n", component->GetId(), request);
                    continue;
                }

                // 같은 프레임의 기존 패치 팔레트를 만들고 신규 고정 슬롯만 A 팔레트로 대체한다.
                std::vector<Joint> palette;
                BuildPatchPalette(skin.Snapshot, source.GetPreparedJoints(), source.GetPreparedNodes(), palette,
                    source.GetPreparedAnimJoints(), source.GetPreparedAnimNodes(), PatchAnimFollowEnabled());
                auto alternate = palette;
                for (size_t k : added)
                    alternate[k] = aPalette[k];
                float maximum = 0;
                size_t compared = 0;

                // 확장 정점은 두 끝점을 각각 skin한 뒤 원래 보간 계수로 합친다.
                for (size_t vertex = 0; vertex < cpu.size() / stride; ++vertex)
                {
                    // section 사이의 예약 공간은 실제 패치 정점이 아니므로 제외한다.
                    if (std::none_of(skin.Sections.begin(), skin.Sections.end(), [vertex](const auto& section)
                        {
                            // 덧셈 넘침 없이 게시 section의 실제 정점 범위를 판정한다.
                            return vertex >= section.FirstVertex && vertex - section.FirstVertex < section.VertexCount;
                        }))
                        continue;

                    // 해당 정점의 일반 또는 확장 레코드를 읽는다.
                    const auto* begin = cpu.data() + vertex * stride;
                    const auto* end = begin + stride;
                    uint32_t interpolation = 0;
                    float t = 0;
                    if (skin.Vertices.Flags & kSkinExtFlag)
                        std::memcpy(&interpolation, end - kSkinExtInternal, sizeof(interpolation));
                    if (interpolation)
                        std::memcpy(&t, end - kSkinExtT, sizeof(t));
                    Math::Vector4 positions[2] = { Math::Vector4(Math::kZero), Math::Vector4(Math::kZero) };
                    bool affected = false;
                    for (uint32_t e = 0; e < (interpolation ? 2u : 1u); ++e)
                    {
                        // 압축 joint와 weight를 복원하고 위치 전체에 회전·스케일·이동을 적용한다.
                        const auto* endpoint = interpolation ? end - kSkinExtEndpoint0 + e * kSkinEndpointSize : begin;
                        float bind[3];
                        uint16_t ids[4], weights[4];
                        std::memcpy(bind, endpoint, sizeof(bind));
                        std::memcpy(ids, interpolation ? endpoint + kSkinEndpointJoints : end - 16, sizeof(ids));
                        std::memcpy(weights, interpolation ? endpoint + kSkinEndpointWeights : end - 8, sizeof(weights));
                        const float total = float(std::max(1u, uint32_t(weights[0]) + weights[1] + weights[2] + weights[3]));
                        const Math::Vector4 point(bind[0], bind[1], bind[2], 1);
                        for (uint32_t k = 0; k < 4; ++k)
                        {
                            const float weight = weights[k] / total * (interpolation ? (e ? t : 1 - t) : 1);
                            if (weight <= 0 || ids[k] >= palette.size())
                                continue;
                            affected |= std::find(added.begin(), added.end(), ids[k]) != added.end();
                            positions[0] = positions[0] + (palette[ids[k]].posXform * point) * weight;
                            positions[1] = positions[1] + (alternate[ids[k]].posXform * point) * weight;
                        }
                    }
                    if (!affected)
                        continue;

                    // 신규 고정 슬롯의 유효 가중치가 있는 정점만 한 번 집계한다.
                    ++compared;
                    const float difference = 1000.0f * float(Math::Length(Math::Vector3(positions[0] - positions[1])));
                    maximum = std::isfinite(difference) ? std::max(maximum, difference) : INFINITY;
                }
                PatchLog("SKINSTALE patch=%llu cut=%llu boundaryMaxMm=%g verticesCompared=%zu newFrozen=%zu\n",
                    component->GetId(), request, maximum, compared, added.size());
            }
        }
    }

    // 절단과 break 없이 같은 owner에 스키닝 가능한 패치와 제거 마스크를 게시한다.
    void SkeletalPatchCopy::PublishRigBonePatch(Scene& scene, const SkeletalPatchResult& result,
        const std::shared_ptr<SkeletalPatchBuffers>& patch)
    {
        // 원본 삭제와 다른 요청의 마스크 변경은 아무것도 게시하지 않고 거절한다.
        auto* owner = scene.FindObject(m_BoneOwner);
        auto* source = owner ? owner->FindMeshComponent(m_SourceMesh) : nullptr;
        if (!result.PatchTriangles || !patch || !source || source->GetGeometry() != m_SourceRevision
            || (source->GetTriangleMask() ? source->GetTriangleMask()->Revision : 0) != m_MaskRevision)
        {
            PatchLog("RigBoneCut rejected: empty patch or source changed\n");
            return;
        }

        // 실패 복구를 위해 기존 마스크 유무와 제거 상태를 보존한다.
        const bool hadMask = source->GetTriangleMask() != nullptr;
        const auto previous = hadMask ? source->GetTriangleMask()->Pending : std::vector<uint32_t>{};
        bool maskTouched = false;
        try
        {
            // 기존 패치 id를 보존하며 새로 추출한 원본 삼각형만 제거한다.
            uint32_t count = 0, patchId = 0;
            for (size_t section = 0; section < m_SourceRevision->GetSections().size(); ++section)
                for (uint32_t draw = 0; draw < m_SourceRevision->GetMesh(section).numDraws; ++draw)
                    count += m_SourceRevision->GetMesh(section).draw[draw].primCount / 3;
            auto removal = hadMask ? previous : std::vector<uint32_t>(count);
            for (uint32_t value : removal) patchId = std::max(patchId, value & 65535u);
            if (++patchId > 65535u) throw std::runtime_error("patch id exhausted");
            for (uint32_t triangle : patch->Triangles) removal.at(triangle) = 0x80000000u | patchId;

            // 준비 중인 컴포넌트는 아직 owner에 공개하지 않는다.
            auto piece = owner->PrepareMeshComponent(GeometryRevision::FromGenerated(patch->Geometry));
            piece->Attachment = std::make_unique<NodeAttachment>(m_BoneAttachment);

            // D5는 cutter를 거치지 않으므로 추출 정점 자체를 복사 출처로 지정한다.
            if (!m_SkinSnapshots.empty() && m_SkinSnapshots[0].SkinningAllowed)
            {
                const uint32_t count = result.PatchTriangles * 3;
                std::vector<uint32_t> origins(size_t(count) * 8, 0);
                for (uint32_t v = 0; v < count; ++v)
                {
                    origins[size_t(v) * 8 + 2] = v;
                    origins[size_t(v) * 8 + 3] = v;
                }
                patch->Geometry->Provenance.Create(L"D5 copied vertex provenance", count, 32, origins.data());
                BuildPieceSkin(*piece, *patch->Geometry, *patch, 0);
            }

            // 게시할 D5만 스키닝 PG를 소유하며 부착 메타데이터와 충돌 정책은 보존한다.
            piece->ActivatePatchSkin();
            piece->SetInheritedCollision({}, piece->GetGeometry()->GetId(), "node attached");
            const auto& instance = *owner->GetModelInstance();
            size_t section = 0;
            while (!source->GetGeometry()->GetMesh(section).numJoints) ++section;
            const auto nodeWorld = JointFrameToWorld(instance, *source, owner->GetTransform(), section)
                * instance.GetNodeTransforms()[m_BoneAttachment.NodeId];
            SceneTransform world;
            if (!SceneTransform::TryFromAffine(Math::AffineTransform(nodeWorld
                * Invert(m_BoneAttachment.NodeWorldAtCut) * m_BoneAttachment.PieceWorldAtCut), world)
                || !piece->SetTransform(world, MeshTransformMode::WorldIndependent))
                throw std::runtime_error("invalid attached world");

            // 확정된 owner pose와 원본 W 및 N으로 게시할 렌더 자원을 먼저 준비한다.
            auto& prepare = GraphicsContext::Begin(L"Prepare rig bone patch");
            piece->Update(prepare, instance, owner->GetTransform(), source);
            prepare.Finish(true);

            // replacement IB 준비 뒤 예약된 자리에 컴포넌트를 게시한다.
            maskTouched = true;
            if (!source->SetTriangleMask(removal)) throw std::runtime_error("invalid removal mask");
            auto& masking = GraphicsContext::Begin(L"Hide rig bone patch source");
            source->Update(masking, instance, owner->GetTransform());
            masking.Finish(true);
            const ComponentId id = piece->GetId();
            m_BonePublication = { owner->GetId(), id, m_Plane, piece->GetEditGeneration(),
                piece->GetGeometry()->GetId(), m_BoneAttachment.NodeId };
            owner->CommitMeshComponent(std::move(piece));
        }
        catch (...)
        {
            // 미게시 컴포넌트는 자동 해제하고 원본 마스크를 요청 전 상태로 되돌린다.
            if (maskTouched)
            {
                if (hadMask) source->SetTriangleMask(previous);
                else source->ClearTriangleMask();
            }
            m_BonePublication = {};
            PatchLog("RigBoneCut rejected: publication failed\n");
            return;
        }

        // 이름 조회나 로그 실패가 완료된 게시를 되돌리지 않게 한다.
        WritePatchLogSafely([&]
        {
            // 후속 단계가 사용할 owner와 컴포넌트 id 및 숨김 결과를 남긴다.
            std::ostringstream line;
            line << "RigBoneCut published owner=" << owner->GetName()
                << " node=" << SafeDiagnosticNodeName(owner->GetName(), m_BoneAttachment.NodeId)
                << " component=" << m_BonePublication.Component << " triangles=" << result.PatchTriangles << " hidden=1\n";
            return line.str();
        });
    }

    // 복사본을 절단하고 break 성공 시 A/B의 전용 스키닝 PG를 게시한다.
    bool SkeletalPatchCopy::Update(Scene& scene, CuttingSystem& cutting, SkeletalPatchResult& result)
    {
        // 압축과 취소 작업은 기존 게시 진행 여부와 관계없이 매 프레임 회수한다.
        PollBodyCompaction();
        PruneDetachHistory(scene);

        // 압축 완료 뒤에만 스키닝과 break를 준비하고 실패하면 기존 side를 사용한다.
        if (m_ReadyCut && !DiscardStaleCutResult(scene))
        {
            if (GetBodyState() == PatchBodyState::InProgress) return false;
            auto ready = std::move(m_ReadyCut);
            FinishCutResult(scene, *ready, m_ReadyCaps, m_ReadyUncapped);
        }

        // D5 추출 완료는 break 취소와 Stale 및 절단 제출 분기에 들어가지 않는다.
        if (m_BonePending)
        {
            std::shared_ptr<SkeletalPatchBuffers> patch;
            if (!m_Job.Poll(result, patch))
                return false;
            CheckPatchSkin(*patch);
            m_BonePending = false;
            PublishRigBonePatch(scene, result, patch);
            return true;
        }

        // CPU는 1프레임, GPU는 2프레임 늦으므로 게시 후 첫 3프레임을 건너뛰고 유효 표본 60개를 평균한다.
        if (m_BoundaryWait && m_BoundaryWait <= 60)
        {
            // 이번 프레임의 두 조각이 모두 유효할 때만 하나의 표본으로 취급한다.
            const auto* owner = scene.FindObject(m_Break.Owner);
            uint32_t pieces = 0;
            uint32_t vertices = 0;
            double cpuTotal = 0.0;
            double gpuTotal = 0.0;
            bool valid = true;

            // 게시된 조각의 고유 타이머를 조회하고 누락 및 비정상 시간을 제외한다.
            for (const auto id : m_PieceIds)
            {
                const auto* piece = owner ? owner->FindMeshComponent(id) : nullptr;
                if (!piece || !piece->HasSkinnedPatch())
                {
                    valid = false;
                    continue;
                }

                // 수집된 CPU/GPU 쌍을 검사한 뒤 프레임 합계에 반영한다.
                float cpuMs = 0.0f;
                float gpuMs = 0.0f;
                const bool collected = EngineProfiling::GetLastBlockTimes(piece->PatchSkinTimingName, cpuMs, gpuMs);
                valid = valid && collected && std::isfinite(cpuMs) && std::isfinite(gpuMs)
                    && cpuMs >= 0.0f && gpuMs >= 0.0f;
                cpuTotal += cpuMs;
                gpuTotal += gpuMs;
                ++pieces;
                vertices += piece->PatchSkin->VertexCount;
            }

            // 무효 프레임은 분모에도 넣지 않고 두 조각의 유효 합계만 누적한다.
            if (valid && pieces == 2 && std::isfinite(cpuTotal) && std::isfinite(gpuTotal))
            {
                m_SkinCpuMs += cpuTotal;
                m_SkinGpuMs += gpuTotal;
                ++m_SkinTimingFrames;
            }

            // 각 case의 마지막 표본 뒤에 양쪽 조각의 프레임당 평균을 한 번만 기록한다.
            if (m_SkinTimingFrames == 60)
                PatchLog("PS7 case=%u pieces=%u vertices=%u cpuAvg_ms=%.4f gpuAvg_ms=%.4f frames=%u\n",
                    m_BreakCheck, pieces, vertices, m_SkinCpuMs / m_SkinTimingFrames,
                    m_SkinGpuMs / m_SkinTimingFrames, m_SkinTimingFrames);

            // 무효 표본만큼 마지막 대기를 연장해 다음 case 전에 유효 표본 60개를 확보한다.
            if (m_BoundaryWait == 1 && m_SkinTimingFrames < 60)
                ++m_BoundaryWait;
        }

        // 경계 검사는 게시 60프레임 뒤에 유지하고 타이밍 수집이 끝난 뒤 다음 case를 허용한다.
        if (m_BoundaryWait)
        {
            --m_BoundaryWait;
            if (m_BoundaryWait == 3)
            {
                MeasureBoundaryAt(scene, "after60");
                if (m_BreakCheck >= 2)
                    m_CaptureRequest = m_BreakCheck - 1;
            }
        }

        // 다중 결과는 batch 전체를 소비하고 단일 결과 소비 경로와 분리한다.
        if (m_RequestId && m_MultiPrepare)
        {
            if (!ConsumeMultiBreak(scene))
            {
                return false;
            }
        }

        // substep이 결과를 만들 때까지 다음 C 진단을 대기시킨다.
        if (m_RequestId)
        {
            BreakStatus status;
            if (!m_Physics->TryGetBreakResult(m_RequestId, status))
                return false;
            CutPerf::Mark("break-result");
            SkeletalRigState state;
            m_Physics->GetSkeletalPhysics().ReadDiagnosticState(m_Break.Owner, state);
            unsigned dynamic = 0;
            for (const auto& body : state.Bodies)
                dynamic += body.Motion == MiniPhysics::BodyMotion::Dynamic;
            PatchLog("SP7b RequestId=%llu BreakStatus=%u JointId=%u ParentNode=%u Dynamic=%u\n",
                m_RequestId, unsigned(status), m_Break.JointId, m_Incoming.ParentNode,
                dynamic >= m_DynamicBefore ? dynamic - m_DynamicBefore : 0);

            // 요청 ID를 초기화하기 전에 게시 진단에 사용할 값을 보존한다.
            const uint64_t publishedRequest = m_RequestId;
            m_LastBreakStatus = status;
            m_RequestId = 0;

            // 성공과 C snapshot 일치 여부를 확인한 뒤 원본 숨김과 원래 owner의 조각을 함께 게시한다.
            if (!m_OffsetCopy)
            {
                auto* owner = scene.FindObject(m_Break.Owner);
                auto* source = owner ? owner->FindMeshComponent(m_SourceMesh) : nullptr;
                auto* staging = scene.FindObject(m_CopyOwner);
                const bool current = source && source->GetGeometry() == m_SourceRevision
                    && (source->GetTriangleMask() ? source->GetTriangleMask()->Revision : 0) == m_MaskRevision;
                if (status == BreakStatus::Applied && current && staging && staging->GetMeshComponents().size() == m_Attachments.size())
                {
                    // 준비된 두 geometry의 원래 배치와 본 snapshot을 게시 전에 구성한다.
                    std::vector<std::unique_ptr<MeshComponent>> pieces;
                    for (size_t i = 0; i < m_Attachments.size(); ++i)
                    {
                        // 원래 배치와 부착 노드에 맞춰 게시할 조각을 준비한다.
                        auto* prepared = staging->FindMeshComponent(m_PreparedIds[i]);
                        auto piece = owner->PrepareMeshComponent(prepared->GetGeometry(), 2);
                        if (!piece->SetTransform(m_World, MeshTransformMode::WorldIndependent))
                            throw std::runtime_error("SP7e invalid piece world");
                        piece->SetInheritedCollision({}, piece->GetGeometry()->GetId(), "node attached");
                        piece->Attachment = std::make_unique<NodeAttachment>(m_Attachments[i]);
                        piece->PatchSkin = prepared->PatchSkin;
                        piece->ActivatePatchSkin();

                        // owner 안에서만 고유한 컴포넌트 ID에 owner ID를 붙여 타이머 충돌을 막는다.
                        if (m_BreakCheck && !m_ForceStale)
                        {
                            m_PieceIds[i] = piece->GetId();
                            piece->PatchSkinTimingName = L"Patch Skinning " + std::to_wstring(owner->GetId())
                                + L"/" + std::to_wstring(piece->GetId());
                        }

                        // 절단 성능 로그가 켜진 경우 프레임별 조각 스키닝 시간을 읽도록 타이머 이름만 지정한다.
                        if (CutPerf::Enabled() && !m_BreakCheck)
                            piece->PatchSkinTimingName = L"Patch Skinning " + std::to_wstring(owner->GetId())
                                + L"/" + std::to_wstring(piece->GetId());

                        // 게시 전 준비 목록에 조각을 보관한다.
                        pieces.push_back(std::move(piece));
                    }
                    if (!m_DemoUnmask && !source->SetTriangleMask(m_Removal))
                        throw std::runtime_error("SP7e invalid removal mask");

                    // 이전 이력을 새 A/B에도 적용하도록 게시 ID를 보관한다.
                    std::vector<ComponentId> newPieces;
                    for (const auto& piece : pieces)
                        newPieces.push_back(piece->GetId());
                    for (auto& piece : pieces)
                        owner->CommitMeshComponent(std::move(piece));
                    PublishDetachRecord(scene, *owner, *source, owner->FindMeshComponent(newPieces.front()), newPieces, publishedRequest);
                    PatchLog("SP7e published owner=%llu patchTriangles=%u hidden=%u nodes=%u/%u\n",
                        m_Break.Owner, m_Result.PatchTriangles, m_DemoUnmask ? 0u : 1u, m_Attachments[0].NodeId, m_Attachments[1].NodeId);

                    // 같은 프레임에 치환 IB와 새 조각의 렌더 상수 준비를 끝낸다.
                    auto& context = GraphicsContext::Begin(L"Publish attached skeletal pieces");
                    scene.PrepareMeshes(context);
                    context.Finish();
                    LogStaleSkin(*owner, state, publishedRequest);
                    CutPerf::Mark("published");

                    // 게시된 조각이 상주시키는 버퍼의 논리·할당 크기를 성능 로그에만 남긴다.
                    if (CutPerf::Enabled())
                    {
                        for (const auto& published : owner->GetMeshComponents())
                        {
                            if (!published->PatchSkin) continue;
                            const auto generated = published->GetGeometry()->GetGenerated();
                            if (!generated) continue;
                            CutPerf::Memory("piece", "Data-VB-depth-IB-shared", generated->Data);
                            CutPerf::Memory("piece", "Provenance", generated->Provenance);
                            CutPerf::Memory("piece", "CapMaterial", generated->CapMaterial);
                            CutPerf::Memory("piece", "SkinVB", published->PatchSkin->Vertices.Data);
                            CutPerf::Memory("piece", "PosedGeometry", published->GetPosedGeometry().GetBuffer());

                            // quiet 실행도 성능 로그에서 CPU 사본이 비어 있는지 확인할 수 있게 한다.
                            CutPerf::Log("SKINCPU patch=%llu bytes=%zu\n", published->GetId(), published->PatchSkin->Vertices.DiagnosticSkin.size());
                        }
                        if (const auto* mask = source->GetTriangleMask())
                        {
                            CutPerf::Memory("source-mask", "State", mask->State);
                            if (mask->CompactIB) CutPerf::Memory("source-mask", "CompactIB", *mask->CompactIB);
                        }
                    }

                    // 시연 캡처는 A/B 게시와 렌더 준비가 완료된 프레임부터 시작한다.
                    if (m_Demo)
                    {
                        m_DemoPublished = true;
                        m_CaptureRequest = m_Demo;
                    }

                    // 실제 게시 프레임의 posed 원본과 부착 조각을 비교한다.
                    if (m_BreakCheck && !m_ForceStale)
                    {
                        MeasureBoundaryAt(scene, "publish");
                        m_BoundaryWait = 63;
                        m_SkinCpuMs = 0.0;
                        m_SkinGpuMs = 0.0;
                        m_SkinTimingFrames = 0;
                    }
                }
                else
                    PatchLog("SP7e discarded BreakStatus=%u revisionMatch=%u originalUntouched=1\n", unsigned(status), current);
                if (staging)
                    scene.DeleteObject(m_CopyOwner);
                m_CopyOwner = 0;
            }
        }

        // 물리 프레임 뒤에도 진단 복사본은 강체 없이 고정된 월드 배치를 유지해야 한다.
        if (m_Diagnostic)
            if (const auto* owner = scene.FindObject(m_CopyOwner))
                for (size_t i = 0; i < owner->GetMeshComponents().size(); ++i)
                {
                    const auto& piece = *owner->GetMeshComponents()[i];
                    const Math::Vector3 expected = m_World.GetTranslation()
                        - (i == 1 ? Math::Normalize(Math::Vector3(m_Plane.WorldPlane)) * 0.1f : Math::Vector3(Math::kZero));
                    if (piece.IsSimulating() || !piece.GetCollision().ConvexElems.empty()
                        || float(Math::Length(piece.GetWorldTransform(owner->GetTransform()).GetTranslation() - expected)) > 1e-5f)
                        throw std::runtime_error("Skeletal patch static placement changed");
                }

        // 최종 결과를 한 번만 반환하고 다음 진단 지역 진행을 허용한다.
        if (m_Completed && !m_RequestId)
        {
            // 게시 후 검사 성공도 결과와 함께 남긴다.
            if (m_Diagnostic && m_OffsetCopy)
                PatchLog("Skeletal patch static placement PASS: no dynamic bodies, offset and gap preserved\n");
            result = m_Result;
            m_Completed = false;
            return true;
        }

        // Static batch 완료 전에는 직접 요청하지 않아 Busy 거절을 피한다.
        if (m_CutPending && !cutting.IsBusy())
        {
            m_CutPending = false;
            auto* owner = scene.FindObject(m_CopyOwner);
            auto* component = owner && !owner->GetMeshComponents().empty() ? owner->GetMeshComponents().front().get() : nullptr;
            if (!component)
            {
                LogCutResult(scene, "copy target deleted", 0, 0, 0);
                return false;
            }

            // 직접 요청 동안만 허용하고 Static 후보 제외 플래그를 즉시 복원한다.
            owner->SetNotCuttable(false);
            try
            {
                m_CutInFlight = cutting.Request(m_CopyOwner, component->GetId(), m_Plane, true, 1.0e-5f, 0, m_SkinPatch != nullptr);
            }
            catch (...)
            {
                owner->SetNotCuttable(true);
                throw;
            }
            owner->SetNotCuttable(true);
            CutPerf::Mark("cut-submitted");
            PatchLog("Skeletal patch cut submitted=%u owner=%llu\n", m_CutInFlight ? 1u : 0u,
                static_cast<unsigned long long>(m_CopyOwner));
            if (!m_CutInFlight)
            {
                // 제출 거절도 완료 실패로 처리하고 숨긴 준비물을 즉시 해제한다.
                LogCutResult(scene, cutting.GetStatusText(), 0, 0, 0);
                if (!m_OffsetCopy)
                {
                    scene.DeleteObject(m_CopyOwner);
                    m_CopyOwner = 0;
                    m_BreakReady = false;
                }
            }
        }

        // 영향 삼각형이 없는 패치만 생략하며 유한 영역 교차 수는 무한 절단을 막지 않는다.
        std::shared_ptr<SkeletalPatchBuffers> patch;
        if (!m_Job.Poll(result, patch))
            return false;
        CutPerf::Mark("extract-polled");
        CheckPatchSkin(*patch);
        m_SkinPatch = !m_SkinSnapshots.empty() ? patch : nullptr;
        m_SelectedNode = m_Job.SelectedNode();
        if (!result.PatchTriangles)
        {
            // 빈 선택 결과는 복사와 절단 요청 없이 사유를 기록한다.
            PatchLog("Skeletal patch publish skipped: no influenced triangles; no copy or cut requested\n");
            PatchLog("Skeletal patch cut result: rejected %s components=0 caps=0 uncapped=0 error=0 gap=0\n",
                "no influenced triangles");
            if (m_Diagnostic && scene.FindObject(m_CopyOwner))
                throw std::runtime_error("Skeletal patch empty selection created a copy");
            return true;
        }

        // 같은 원본 재질을 쓰는 자동 생성 컴포넌트의 geometry만 교체한다.
        m_Result = result;
        if (m_Diagnostic && !m_OffsetCopy)
        {
            m_Boundary = patch->Boundary;
            m_BoundarySkins = patch->BoundarySkins;
        }

        // 원본 전역 삼각형 번호로 기존 마스크를 보존하며 새 패치 id를 배정한다.
        if (!m_OffsetCopy)
        {
            const auto* original = scene.FindObject(m_Break.Owner);
            const auto* source = original ? original->FindMeshComponent(m_SourceMesh) : nullptr;
            if (!source || source->GetGeometry() != m_SourceRevision
                || (source->GetTriangleMask() ? source->GetTriangleMask()->Revision : 0) != m_MaskRevision)
            {
                PatchLog("SP7e discarded before break: revision mismatch\n");
                return true;
            }

            // 기존 마스크와 합칠 전역 삼각형 수 및 패치 ID를 계산한다.
            uint32_t count = 0;
            uint32_t patchId = 0;
            for (size_t s = 0; s < m_SourceRevision->GetSections().size(); ++s)
                for (uint32_t d = 0; d < m_SourceRevision->GetMesh(s).numDraws; ++d)
                    count += m_SourceRevision->GetMesh(s).draw[d].primCount / 3;
            m_Removal = source->GetTriangleMask() ? source->GetTriangleMask()->Pending : std::vector<uint32_t>(count);
            for (uint32_t value : m_Removal)
                patchId = std::max(patchId, value & 65535u);
            if (++patchId > 65535u)
            {
                PatchLog("SP7e rejected: patch id exhausted\n");
                return true;
            }
            for (uint32_t triangle : patch->Triangles)
                m_Removal.at(triangle) = 0x80000000u | patchId;
        }

        // 복사본의 월드 오프셋과 생성 geometry를 게시한다.
        m_Offset = m_OffsetCopy ? patch->SourceWorldWidth + 0.5f : 0;
        m_World.SetTranslation(m_World.GetTranslation() + Math::Vector3(m_Offset, 0, 0));
        auto revision = GeometryRevision::FromGenerated(patch->Geometry);
        auto& owner = scene.CreateObject(m_Name, revision->GetSource());
        m_CopyOwner = owner.GetId();
        if (!owner.GetMeshComponents().front()->SetGeometry(std::move(revision)) || !owner.SetTransform(m_World))
            throw std::runtime_error("Skeletal patch publication failed");
        owner.SetNotCuttable(true);
        owner.GetMeshComponents().front()->PublishVisible = m_OffsetCopy;

        // C 시점 평면 변환을 복사본과 같은 거리로 옮겨 모든 평면 정보를 재구축한다.
        Math::AffineTransform planeWorld(Invert(m_Plane.WorldToPlane));
        planeWorld.SetTranslation(planeWorld.GetTranslation() + Math::Vector3(m_Offset, 0, 0));

        // 추출 GPU 경계 구를 월드로 옮겨 평면 XZ 투영과 여유를 모두 덮는다.
        const auto& packedMesh = patch->Geometry->Meshes.front();
        const Math::Vector3 localCenter(packedMesh.bounds[0], packedMesh.bounds[1], packedMesh.bounds[2]);
        const Math::Vector3 worldCenter = m_World.ToAffine() * localCenter;
        const auto scale = m_World.GetScale();
        const float radius = packedMesh.bounds[3] * std::max({float(scale.GetX()), float(scale.GetY()), float(scale.GetZ())});
        const Math::AffineTransform worldToPlane(Invert(Math::Matrix4(planeWorld)));
        const Math::Vector3 relative = worldToPlane * worldCenter;
        const float margin = std::max(0.01f, radius * 0.05f);

        // 평면 자체의 비균일 scale도 역변환하여 월드 경계와 여유가 축소되지 않게 한다.
        const float projectedRadius = (radius + margin) * AffineRadiusScale(worldToPlane);
        const float width = 2 * (std::abs(float(relative.GetX())) + projectedRadius);
        const float height = 2 * (std::abs(float(relative.GetZ())) + projectedRadius);
        if (!MeshCutting::TryBuildCutPlaneData({ width, height }, planeWorld, m_Plane))
            throw std::runtime_error("Skeletal patch offset plane invalid");
        PatchLog("Skeletal patch infinite cut rectangle=(%.6f,%.6f) worldMargin=%.6f offset=%.6f\n",
            width, height, margin, m_Offset);

        // 실제 절단 요청과 같은 평면으로 라벨을 보관하며 기존 절단 출력은 그대로 둔다.
        m_Job.DiagnoseIslands(*patch, m_Plane, m_Offset, m_BodySides[0] == 0, m_ModelName);

        // 생성 geometry의 pose와 bounds를 준비한 뒤 다음 프레임의 절단 요청을 허용한다.
        scene.InitializePoses();
        if (!scene.FinalizeObjectPose(m_CopyOwner, nullptr, 0))
            throw std::runtime_error("Skeletal patch copy pose invalid");
        auto& context = GraphicsContext::Begin(L"Prepare skeletal patch copy");
        owner.GetMeshComponents().front()->Update(context, *owner.GetModelInstance(), owner.GetTransform());
        context.Finish();
        m_CutPending = true;
        CutPerf::Mark("copy-prepared");

        // Static batch·벤치마크·자동 재절단과 격리된 owner가 정확히 하나인지 검사한다.
        size_t copies = 0;
        for (const auto& object : scene.GetObjects())
            if (object->GetName().find("_SkeletalPatch") != std::string::npos)
                ++copies;
        PatchLog("Skeletal patch published owner=%s triangles=%u worldOffset=(%.6f,0,0) copies=%zu %s\n",
            owner.GetName().c_str(), result.PatchTriangles, m_Offset, copies, copies == 1 ? "PASS" : "FAIL");
        if (copies != 1)
            throw std::runtime_error("Skeletal patch copy isolation failed");
        return false;
    }

    // 교차점을 포함하는 실제 loop의 테두리 아일랜드를 GPU 분류 전에 재라벨한다.
    static std::set<uint32_t> RelabelCapLoops(const CutResultEntry& result, SkeletalPatchBuffers& patch,
        const MeshCutting::CutPlaneData& plane)
    {
        // 함수 전체 CPU 시간을 기존 성능 로그 옵션에서만 기록한다.
        CutPerf::Cpu cutPerfCpu("RelabelCapLoops");

        // cutter와 같은 월드 평면 축으로 snapshot 선분과 교차점을 비교한다.
        const Math::Vector3 normal(plane.WorldPlane);
        const auto u = Normalize(Cross(normal, Abs(normal.GetY()) < 0.9f
            ? Math::Vector3(Math::kYUnitVector) : Math::Vector3(Math::kXUnitVector)));
        const auto v = Cross(normal, u);
        std::map<uint32_t, std::vector<std::array<float, 4>>> loops;
        std::set<uint32_t> bad, real;
        for (const auto& segment : result.CapSegments)
        {
            if (!segment[2]) continue;
            std::array<float, 4> edge;
            std::memcpy(edge.data(), segment.data() + 4, sizeof(edge));
            loops[segment[1]].push_back(edge);
            if (segment[3] != 0) bad.insert(segment[1]);
        }

        // 각 교차점의 최소 loop를 고르고 같은 loop를 요구하는 P_i는 진단 실패로 격리한다.
        for (uint32_t leader : bad)
        {
            loops.erase(leader);
        }
        std::map<uint32_t, uint32_t> owners;
        for (uint32_t i = 0; i < patch.Crossings.size(); ++i)
        {
            const auto& crossing = patch.Crossings[i];
            const std::array<float, 2> point = { Dot(u, crossing.Point), Dot(v, crossing.Point) };
            const uint32_t selected = crossing.Valid ? SmallestContainingLatticeLoop(loops, point) : UINT32_MAX;
            PatchLog("MI4d2 body=%u P=%u child=%u crossingValid=%u crossing=(%g,%g,%g) childSide=%u loop=%u\n",
                i + 1, crossing.Root, crossing.Child, crossing.Valid, float(crossing.Point.GetX()),
                float(crossing.Point.GetY()), float(crossing.Point.GetZ()), crossing.ChildSide, selected);
            if (selected == UINT32_MAX)
            {
                if (patch.Crossings.size() > 1)
                {
                    PatchLog("MI4d2 diagnostic FAIL body=%u missing real loop\n", i + 1);
                    throw std::runtime_error("MI4d2 missing multi-P loop");
                }
                continue;
            }
            if (!owners.emplace(selected, i).second)
            {
                PatchLog("MI4d2 diagnostic FAIL loop=%u shared bodies=%u,%u\n", selected, owners.at(selected) + 1, i + 1);
                throw std::runtime_error("MI4d2 shared multi-P loop");
            }
            real.insert(selected);
        }

        // 실제 loop의 자식 side는 B_i, 반대 side는 A로 요청하고 아일랜드 충돌을 함께 검사한다.
        std::vector<std::array<uint32_t, 3>> requests;
        for (const auto& segment : result.CapSegments)
        {
            if (!segment[2] || !real.count(segment[1]))
            {
                continue;
            }
            const uint32_t owner = owners.at(segment[1]);
            for (uint32_t side = 0; side < 2; ++side)
            {
                requests.push_back({ segment[0], side, side == patch.Crossings[owner].ChildSide ? owner + 1 : 0 });
            }
        }
        IslandLabels relabeled = patch.Islands;

        // 아일랜드 재라벨링 호출의 CPU 시간을 측정한다.
        CutPerf::Cpu cutPerfRelabel("RelabelPatchIslands");
        const auto counts = RelabelPatchIslands(relabeled, requests);
        cutPerfRelabel.End();

        PatchLog("MI3e2 realLoops=%zu relabeled islands=%u tris=%u conflicts=%u\n",
            real.size(), counts[0], counts[1], counts[2]);
        if (patch.Crossings.size() > 1 && counts[2])
        {
            PatchLog("MI4d2 diagnostic FAIL conflicting islands=%u; original labels preserved\n", counts[2]);
            throw std::runtime_error("MI4d2 conflicting multi-P island requests");
        }
        patch.Islands = std::move(relabeled);
        return real;
    }

    // 재라벨한 경계 몸체를 투표하고 leader별 GPU 귀속 표를 만든다.
    static std::vector<std::array<uint32_t, 2>> DiagnoseCapLoops(const CutResultEntry& result, const IslandLabels& islands,
        const std::set<uint32_t>& real, uint32_t bodyCount)
    {
        // 함수 전체 CPU 시간을 기존 성능 로그 옵션에서만 기록한다.
        CutPerf::Cpu cutPerfCpu("DiagnoseCapLoops");

        // 유효한 loop의 각 선분에서 양쪽 몸체 투표와 미해결 수를 집계한다.
        struct Votes
        {
            uint32_t Segments = 0, Missing[2] = {};
            std::map<uint32_t, uint32_t> Side[2];
        };
        std::map<uint32_t, Votes> loops;
        for (const auto& segment : result.CapSegments)
        {
            if (!segment[2])
                continue;
            auto& votes = loops[segment[1]];
            ++votes.Segments;
            for (uint32_t side = 0; side < 2; ++side)
            {
                const uint32_t body = segment[0] < islands.TriangleSideBody.size()
                    ? islands.TriangleSideBody[segment[0]][side] : UINT32_MAX;
                if (body == UINT32_MAX || (segment[3] & 5) != 0)
                    ++votes.Missing[side];
                else
                    ++votes.Side[side][body];
            }
        }

        // 최다 득표 몸체를 고르고 동점은 작은 몸체 번호, 출처 부재는 SideDefault로 기록한다.
        std::vector<std::array<uint32_t, 2>> table(1, { UINT32_MAX, UINT32_MAX });
        uint32_t drop = 0, conflicts = 0, fallback = 0;
        for (const auto& loop : loops)
        {
            const auto& votes = loop.second;
            uint32_t bodies[2], disagreement = 0;
            for (uint32_t side = 0; side < 2; ++side)
            {
                bodies[side] = islands.SideDefault[side];
                uint32_t best = 0, valid = 0;
                for (const auto& vote : votes.Side[side])
                {
                    valid += vote.second;
                    if (vote.second > best)
                    {
                        best = vote.second;
                        bodies[side] = vote.first;
                    }
                }
                disagreement += valid - best;
                fallback += votes.Missing[side];
            }

            // 실제 분리 loop만 유지하고 나머지는 양쪽 모두 drop 전용 값으로 표시한다.
            conflicts += disagreement;
            const bool keep = real.count(loop.first) && bodies[0] != bodies[1];
            if (!keep)
            {
                ++drop;
            }
            if (table.size() <= loop.first)
            {
                table.resize(size_t(loop.first) + 1, { UINT32_MAX, UINT32_MAX });
            }
            table[loop.first] = keep ? std::array<uint32_t, 2>{ bodies[0], bodies[1] }
                : std::array<uint32_t, 2>{ UINT32_MAX - 1, UINT32_MAX - 1 };
            const std::string action = !keep ? "drop" :
                "keep(side0->" + std::to_string(bodies[0]) + ",side1->" + std::to_string(bodies[1]) + ")";
            PatchLog("MI3e loop=%u segs=%u side0Body=%u side1Body=%u conflicts=%u action=%s\n",
                loop.first, votes.Segments, bodies[0], bodies[1], disagreement, action.c_str());
        }

        // fallback은 출처가 없어 투표에서 빠진 (선분, side) 수다.
        PatchLog("MI3e loops=%u drop=%u keep=%u conflicts=%u fallback=%u\n",
            uint32_t(loops.size()), drop, uint32_t(loops.size()) - drop, conflicts, fallback);

        // 다중 요청은 불완전한 loop 투표를 side 기본값으로 압축하지 않고 진단 실패로 격리한다.
        if (bodyCount > 2 && (conflicts || fallback))
        {
            PatchLog("MI4d2 diagnostic FAIL cap conflicts=%u fallback=%u\n", conflicts, fallback);
            throw std::runtime_error("MI4d2 ambiguous multi-P cap labels");
        }

        // 다중 몸체는 side별 유지 loop 수를 몸체마다 세어 같은 side B1/B2 cap 분리를 기록한다.
        if (bodyCount > 2)
        {
            bool split = false;
            for (uint32_t side = 0; side < 2; ++side)
            {
                std::vector<uint32_t> perBody(bodyCount, 0);
                for (const auto& entry : table)
                {
                    if (entry[side] < bodyCount)
                    {
                        ++perBody[entry[side]];
                    }
                }
                std::string counts;
                for (uint32_t body = 0; body < bodyCount; ++body)
                {
                    counts += " body" + std::to_string(body) + "=" + std::to_string(perBody[body]);
                }
                split |= perBody[1] && perBody[2];
                PatchLog("MI4d2 cap loops side=%u%s\n", side, counts.c_str());
            }
            PatchLog("MI4d2 same-side B1/B2 cap split=%s\n", split ? "PASS" : "FAIL");
        }
        return table;
    }

    struct SkeletalPatchCopy::BodyCompactJob
    {
        std::shared_ptr<const GeometryBuffers> Inputs[2];
        Microsoft::WRL::ComPtr<ID3D12Resource> Bodies[2];
        std::shared_ptr<GeometryBuffers> Output;
        ByteAddressBuffer Layouts, Bins, Status;
        ReadbackBuffer Metadata, Flags;
        ByteAddressBuffer ClassifyCounters[2];
        ReadbackBuffer ClassifyReadback[2];
        Microsoft::WRL::ComPtr<ID3D12Resource> ClassifyProvenance[2], ClassifyTables[4];
        ByteAddressBuffer Overrides;
        std::vector<std::array<uint32_t, 2>> OverrideData;
        bool GpuClassify = false, VerifyClassify = false;
        bool Classified[2] = {};
        std::map<uint32_t, Mesh> Templates;
        std::vector<uint32_t> LayoutData;
        uint64_t Fence = 0, Triangles = 0, Bytes = 0, Dropped = 0;
        uint32_t BodyCount = 0;

        // 일반 회수는 완료 확인 후 수행하고 종료 시에만 미완료 제출을 보호한다.
        ~BodyCompactJob()
        {
            // 소유자 종료 시에도 제출 자원의 수명을 보장한다.
            if (Fence && !Graphics::g_CommandManager.IsFenceComplete(Fence))
                Graphics::g_CommandManager.WaitForFence(Fence);

            // 미게시 출력은 전역 대기를 일으키는 소멸자보다 먼저 해제한다.
            if (Output) Output->ReleaseGpuResourcesAfterCompletion();
        }
    };

    // cutter 출력의 실제 section 범위를 n+1 몸체로 판정하고 압축용 표를 보관한다.
    // 승인된 GPU 표에 섬별 cap 보정을 적용하고 검증 카운터는 압축 완료 후 읽는다.
    void SkeletalPatchCopy::ClassifyCutBodies(const GeometryBuffers& geometry, const IslandLabels& islands,
        const std::vector<std::array<uint32_t, 2>>& capLoops, uint32_t side, uint32_t bodyCount)
    {
        // 합성 압축 검사는 진단 옵션에서만 실행한다.
        CheckBodyCompaction();

        // 몸체 수는 라벨 최댓값이 아닌 요청 그룹 수이며 빈 마지막 몸체도 포함한다.
        if (!bodyCount || bodyCount > (UINT32_MAX / 4 - 10) / 2)
            throw std::length_error("MI4d invalid classification body count");

        // 표는 [패치 삼각형][반공간] 순서이며 물리 A/B 순서와 무관하다.
        if (side == 0)
            DiscardBodyCompaction();
        if (islands.TriangleSideBody.empty())
            return;
        if (side == 0)
        {
            // 분류부터 압축 완료까지 하나의 job으로 보호한다.
            m_BodyJob = std::make_shared<BodyCompactJob>();
            m_BodyJob->BodyCount = bodyCount;

            // 보정은 섬 전체의 몸체 변경만 허용하고 작은 요청 표로 제한한다.
            auto& job = *m_BodyJob;
            job.GpuClassify = m_Job.m_ClassifyGpuReady;
            if (job.GpuClassify)
            {
                const auto& before = m_Job.m_IslandLabels;
                bool compact = islands.Islands.size() == before.Islands.size()
                    && islands.TriangleSideIsland == before.TriangleSideIsland
                    && islands.TriangleSideBody.size() == before.TriangleSideBody.size();
                for (uint32_t i = 0; compact && i < islands.Islands.size(); ++i)
                    if (islands.Islands[i].Body != before.Islands[i].Body)
                        job.OverrideData.push_back({ i, islands.Islands[i].Body });
                for (size_t t = 0; compact && t < islands.TriangleSideBody.size(); ++t)
                    for (uint32_t s = 0; s < 2; ++s)
                    {
                        const uint32_t id = islands.TriangleSideIsland[t][s];
                        const uint32_t expected = id < islands.Islands.size()
                            ? islands.Islands[id].Body : before.TriangleSideBody[t][s];
                        compact &= expected == islands.TriangleSideBody[t][s];
                    }
                job.GpuClassify = compact && job.OverrideData.size() <= 64;
                if (!job.GpuClassify)
                    Utility::Printf("Island GPU classify fallback reason=%s overrides=%zu\n",
                        compact ? "override-limit" : "non-island-correction", job.OverrideData.size());
                else
                {
                    // verify 전용 CPU 표 외에는 전체 삼각형 표를 다시 올리지 않는다.
                    uint32_t verify = 0;
                    CommandLineArgs::GetInteger(L"island-gpu-verify", verify);
                    job.VerifyClassify = verify != 0;
                    job.Overrides.Create(L"Island body overrides", uint32_t(std::max(size_t(1), job.OverrideData.size())), 8);
                    Utility::Printf("Island GPU classify consumed: overrides=%zu bytes=%zu\n",
                        job.OverrideData.size(), job.OverrideData.size() * 8);
                }
            }

            // job 소유 표를 초기 데이터 없이 만들고 제거된 동기 대기 구간은 유지한다.
            m_CapLoopTable.Create(L"Cap loop side bodies", uint32_t(capLoops.size()), sizeof(capLoops[0]));
            if (!job.GpuClassify || job.VerifyClassify)
                m_IslandTable.Create(L"Island triangle sides", uint32_t(islands.TriangleSideBody.size()),
                    sizeof(islands.TriangleSideBody[0]));

            // 기존 두 업로드 대기가 없어졌음을 같은 이름과 횟수로 기록한다.
            CutPerf::Cpu cutPerfUpload("ClassifyCutBodies-upload-wait");
            cutPerfUpload.End();
            CutPerf::Cpu cutPerfIslandUpload("ClassifyCutBodies-upload-wait");
            cutPerfIslandUpload.End();
        }
        struct alignas(16) Constants
        {
            uint32_t FirstTriangle, TriangleCount, PatchTriangles, Side;
            uint32_t DefaultBody, CapLoopCount, BodyCount, UseGpu;
            uint32_t OverrideCount, Verify, Padding[2];
        };
        static_assert(sizeof(Constants) == 48, "Body classification constants");

        // 읽기 전용 provenance·표와 판정 결과·카운터만 compute에 바인딩한다.
        const bool gpuClassify = m_BodyJob->GpuClassify;
        static RootSignature roots[2];
        static ComputePSO cpuPso(L"Classify patch output bodies"), gpuPso(L"Classify GPU island bodies");
        auto& root = roots[gpuClassify];
        auto& pso = gpuClassify ? gpuPso : cpuPso;
        static bool ready[2] = {};
        auto& initialized = ready[gpuClassify];
        if (!initialized)
        {
            root.Reset(gpuClassify ? 9 : 6);
            root[0].InitAsConstantBuffer(0);
            root[1].InitAsBufferSRV(0);
            root[2].InitAsBufferSRV(1);
            root[3].InitAsBufferUAV(0);
            root[4].InitAsBufferUAV(1);
            root[5].InitAsBufferSRV(2);
            if (gpuClassify)
            {
                // CPU 변형에는 추가 루트 바인딩과 GPU 보정 분기를 넣지 않는다.
                root[6].InitAsBufferSRV(3);
                root[7].InitAsBufferSRV(4);
                root[8].InitAsBufferSRV(5);
            }
            root.Finalize(L"Patch body classification");
            pso.SetRootSignature(root);
            if (gpuClassify)
                pso.SetComputeShader(g_pPatchBodyClassifyGpuCS, sizeof(g_pPatchBodyClassifyGpuCS));
            else
                pso.SetComputeShader(g_pPatchBodyClassifyCS, sizeof(g_pPatchBodyClassifyCS));
            pso.Finalize();
            initialized = true;
        }

        // cutter의 section 사이 capacity 간격을 유지해 globalVertex/3로 결과를 찾게 한다.
        uint32_t end = 0;
        for (const auto& mesh : geometry.Meshes)
        {
            if (mesh.vbStride != 28 || mesh.vbOffset % 84 || mesh.vbSize % 84
                || (uint64_t(mesh.vbOffset) + mesh.vbSize) / 28 * 32 > geometry.Provenance.GetBufferSize())
                throw std::runtime_error("Invalid body classification provenance range");
            end = std::max(end, mesh.vbOffset / 84 + mesh.vbSize / 84);
        }

        // 분류 입력과 출력 참조도 보관해 압축 전 예외와 취소에서 수명을 보장한다.
        auto& job = *m_BodyJob;
        job.ClassifyTables[0] = job.GpuClassify ? m_Job.m_IslandVote.GetResource() : m_IslandTable.GetResource();
        if (job.GpuClassify)
        {
            // 압축 fence까지 원본 GPU 표의 COM 참조를 유지해 취소·다음 요청도 보호한다.
            job.ClassifyTables[2] = m_Job.m_TriangleSideIsland.GetResource();
            if (job.VerifyClassify)
                job.ClassifyTables[3] = m_IslandTable.GetResource();
        }
        job.ClassifyTables[1] = m_CapLoopTable.GetResource();
        job.ClassifyProvenance[side] = const_cast<ByteAddressBuffer&>(geometry.Provenance).GetResource();
        auto& bodies = m_TriangleBodies[side];
        auto& counters = job.ClassifyCounters[side];
        auto& readback = job.ClassifyReadback[side];
        const uint32_t counterCount = 10 + bodyCount * 2 + (job.VerifyClassify ? bodyCount + 1 : 0);
        if (end)
        {
            // 할당 실패가 분류 command context를 열기 전에 기존 예외 경로로 전달되게 한다.
            bodies.Create(L"Output triangle bodies", end, sizeof(uint32_t));
            counters.Create(L"Body classification counters", counterCount, sizeof(uint32_t));
            readback.Create(L"Body classification readback", counterCount, sizeof(uint32_t));
            job.Bodies[side] = bodies.GetResource();
        }

        // 각 section의 실제 출력 삼각형만 dispatch하고 합산 카운터를 초기화한다.
        auto& graphics = GraphicsContext::Begin(L"Classify patch output bodies");
        CutPerf::Gpu cutPerfClassify(graphics, "BodyClassify-whole");
        try
        {
            // side 0의 두 표를 분류 command list에 복사하고 기존 SRV 전이로 이어간다.
            if (side == 0)
            {
                // 원본 바이트를 context 소유 업로드 메모리에 보관해 GPU 완료까지 유지한다.
                const auto uploadTable = [&graphics](GpuBuffer& destination, const void* data, size_t bytes)
                {
                    // 복사 목적지 상태로 전이하고 업로드 메모리의 전체 표를 복사한다.
                    DynAlloc upload = graphics.ReserveUploadMemory(bytes);
                    std::memcpy(upload.DataPtr, data, bytes);
                    graphics.TransitionResource(destination, D3D12_RESOURCE_STATE_COPY_DEST);
                    graphics.CopyBufferRegion(destination, 0, upload.Buffer, upload.Offset, bytes);
                };
                uploadTable(m_CapLoopTable, capLoops.data(), capLoops.size() * sizeof(capLoops[0]));
                if (!job.GpuClassify || job.VerifyClassify)
                    uploadTable(m_IslandTable, islands.TriangleSideBody.data(),
                        islands.TriangleSideBody.size() * sizeof(islands.TriangleSideBody[0]));
                if (job.GpuClassify && !job.OverrideData.empty())
                    uploadTable(job.Overrides, job.OverrideData.data(), job.OverrideData.size() * 8);
            }

            // 빈 side 0도 표 복사를 제출해 이후 side 1의 분류 입력을 보존한다.
            if (!end)
            {
                // 업로드 메모리는 제출 fence까지 유지하며 CPU에서 완료를 기다리지 않는다.
                cutPerfClassify.End();
                job.Fence = graphics.Finish(false);
                return;
            }

            // 복사 완료 후 provenance와 표는 SRV, 판정 결과와 카운터는 UAV로 사용한다.
            auto& compute = graphics.GetComputeContext();
            auto& provenance = const_cast<ByteAddressBuffer&>(geometry.Provenance);
            compute.TransitionResource(provenance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            auto& bodyTable = job.GpuClassify ? static_cast<GpuBuffer&>(m_Job.m_IslandVote) : m_IslandTable;
            compute.TransitionResource(bodyTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if (job.GpuClassify)
            {
                // 추출·cutter·분류는 DIRECT 큐 순서이며 원본 표는 이 시점부터 읽기 전용이다.
                compute.TransitionResource(m_Job.m_TriangleSideIsland, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                compute.TransitionResource(job.Overrides, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                if (job.VerifyClassify)
                    compute.TransitionResource(m_IslandTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            compute.TransitionResource(m_CapLoopTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            compute.TransitionResource(bodies, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.TransitionResource(counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.ClearUAV(counters);
            compute.InsertUAVBarrier(counters);
            compute.SetRootSignature(root);
            compute.SetPipelineState(pso);
            compute.SetBufferSRV(1, provenance);
            const uint64_t tableOffset = job.GpuClassify
                ? uint64_t(m_Job.m_VoteCapacity) * 2 * (m_Job.m_VoteGroups + 5) * 4 : 0;
            compute.SetBufferSRV(2, bodyTable, tableOffset);
            if (job.GpuClassify)
            {
                // 섬 번호와 보정 목록을 별도 SRV로 읽고 verify에서만 CPU 기준 표를 묶는다.
                compute.SetBufferSRV(6, m_Job.m_TriangleSideIsland);
                compute.SetBufferSRV(7, job.Overrides);
                compute.SetBufferSRV(8, job.VerifyClassify ? static_cast<GpuBuffer&>(m_IslandTable) : bodyTable,
                    job.VerifyClassify ? 0 : tableOffset);
            }
            compute.SetBufferSRV(5, m_CapLoopTable);
            compute.SetBufferUAV(3, bodies);
            compute.SetBufferUAV(4, counters);
            for (const auto& mesh : geometry.Meshes)
            {
                Constants c{ mesh.vbOffset / 84, mesh.vbSize / 84, uint32_t(islands.TriangleSideBody.size()),
                    side, islands.SideDefault[side], uint32_t(capLoops.size()), bodyCount, uint32_t(job.GpuClassify),
                    uint32_t(job.OverrideData.size()), uint32_t(job.VerifyClassify), {} };
                if (!c.TriangleCount)
                    continue;
                compute.SetDynamicConstantBufferView(0, gpuClassify ? sizeof(c) : 32, &c);
                const uint32_t groups = (c.TriangleCount + 63) / 64;
                compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
            }

            // job 자원에 복사를 예약하고 provenance와 판정 결과를 후속 읽기 상태로 돌린다.
            graphics.CopyBuffer(readback, counters);
            graphics.TransitionResource(provenance, D3D12_RESOURCE_STATE_GENERIC_READ);
            graphics.TransitionResource(bodies, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            cutPerfClassify.End();
            job.Fence = graphics.Finish(false);
            job.Classified[side] = true;

        }
        catch (...)
        {
            // 기록 중 예외도 제출 fence를 남겨 부분 복사 자원을 안전하게 회수한다.
            cutPerfClassify.End();
            job.Fence = graphics.Finish(false);
            throw;
        }

        // 제출 후 로그 예외는 context 재제출 없이 기존 실패 경로로 전달한다.
        CutPerf::Cpu cutPerfWait("ClassifyCutBodies-counter-wait");
        cutPerfWait.End();
    }

    // 현재 결과를 버리고 미완료 작업은 후속 프레임에서 회수한다.
    void SkeletalPatchCopy::DiscardBodyCompaction()
    {
        // 게시 revision이 참조하는 출력은 유지하고 미게시 출력만 직접 해제한다.
        if (m_BodyResult.Geometry && m_BodyResult.Geometry.use_count() == 1)
            m_BodyResult.Geometry->ReleaseGpuResourcesAfterCompletion();
        m_BodyResult = {};
        m_BodyState = PatchBodyState::None;

        // 제출된 작업은 완료 확인까지 입력과 함께 보관한다.
        if (m_BodyJob && m_BodyJob->Fence
            && !Graphics::g_CommandManager.IsFenceComplete(m_BodyJob->Fence))
            m_RetiredBodyJobs.push_back(m_BodyJob);
        m_BodyJob.reset();
    }

    // 같은 DIRECT 큐의 분류와 압축 완료 후 카운터·몸체 메타데이터를 검증한다.
    void SkeletalPatchCopy::PollBodyCompaction()
    {
        // 무효화된 제출은 완료된 것만 비동기로 해제한다.
        for (auto it = m_RetiredBodyJobs.begin(); it != m_RetiredBodyJobs.end();)
            if (Graphics::g_CommandManager.IsFenceComplete((*it)->Fence))
                it = m_RetiredBodyJobs.erase(it);
            else ++it;
        if (!m_BodyJob || !Graphics::g_CommandManager.IsFenceComplete(m_BodyJob->Fence)) return;

        // 검증 예외도 압축만의 실패로 처리하여 기존 게시 경로를 유지한다.
        try
        {
            auto& job = *m_BodyJob;

            // 압축 fence는 앞선 두 분류 제출도 포함하므로 동기 대기 없이 회수한다.
            for (uint32_t side = 0; side < 2; ++side)
            {
                if (!job.Classified[side])
                    continue;
                auto& readback = job.ClassifyReadback[side];

                // 성공한 매핑은 로그·문자열 할당 예외가 나도 해제한다.
                const auto unmap = [](ReadbackBuffer* buffer)
                {
                    // 완료된 분류 카운터의 읽기 매핑을 해제한다.
                    buffer->Unmap();
                };
                const auto* counts = static_cast<const uint32_t*>(readback.Map());
                const std::unique_ptr<ReadbackBuffer, decltype(unmap)> mapped(&readback, unmap);
                if (job.VerifyClassify)
                {
                    // cap은 공통 표를 쓰며 표면만 이중 판정하고 최종 몸체별 합계를 비교한다.
                    uint32_t mismatch = counts[10 + job.BodyCount * 3];
                    std::string totals;
                    for (uint32_t body = 0; body < job.BodyCount; ++body)
                    {
                        const uint32_t gpu = counts[10 + body];
                        const uint32_t cpu = counts[10 + job.BodyCount * 2 + body];
                        const uint32_t cap = counts[10 + job.BodyCount + body];
                        mismatch += gpu != cpu;
                        totals += " body" + std::to_string(body) + "=" + std::to_string(gpu + cap)
                            + "/" + std::to_string(cpu + cap);
                    }
                    Utility::Printf("Island GPU classify %s: side=%u triangleMismatch=%u%s\n",
                        mismatch ? "FAIL" : "PASS", side, counts[10 + job.BodyCount * 3], totals.c_str());
                }
                PatchLog("MI3a side=%u tris=%u cap=%u body0=%u body1=%u invalid=%u idMismatch=%u reassigned=%u\n",
                    side, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6]);
                job.Dropped += counts[7];
                PatchLog("MI3f side=%u cap=%u dropped=%u kept=%u fallback=%u invalid=%u\n",
                    side, counts[1], counts[7], counts[1] - counts[7], counts[8], counts[9]);

                // 같은 side의 여러 몸체를 기존 순서와 값으로 기록한다.
                for (uint32_t body = 0; body < job.BodyCount; ++body)
                {
                    PatchLog("MI4d classify side=%u body=%u surface=%u cap=%u\n",
                        side, body, counts[10 + body], counts[10 + job.BodyCount + body]);
                }
            }

            // 기존 압축 실패 플래그와 출력 합계 검증은 그대로 유지한다.
            const auto& templates = job.Templates;
            const auto& layouts = job.LayoutData;
            auto output = job.Output;
            const uint32_t bodies = job.BodyCount, sections = uint32_t(templates.size());
            const uint64_t bins = uint64_t(bodies) * sections, triangles = job.Triangles, bytes = job.Bytes;
            const uint32_t flags = *static_cast<const uint32_t*>(job.Flags.Map());
            job.Flags.Unmap();
            std::vector<uint32_t> metadataCopy(bins * 12);
            std::memcpy(metadataCopy.data(), job.Metadata.Map(), metadataCopy.size() * sizeof(uint32_t));
            job.Metadata.Unmap();
            const auto* metadata = metadataCopy.data();
            PatchBodyResult result;
            result.Geometry = output;
            result.Sections.resize(bodies);
            result.Triangles.resize(bodies);
            bool valid = flags == 0;
            uint64_t sum = 0;

            // section별 재질·범위·공통 출처 주소와 GPU AABB를 검증하고 기존 Mesh 형식으로 기록한다.
            for (uint32_t body = 0; body < bodies && !flags; ++body)
            {
                Math::Vector3 low(FLT_MAX), high(-FLT_MAX);
                uint32_t section = 0;
                for (const auto& entry : templates)
                {
                    const uint32_t bin = body * sections + section++;
                    const uint32_t* m = metadata + bin * 6;
                    if (!m[0]) continue;
                    Mesh mesh = entry.second;
                    mesh.vbOffset = m[2];
                    mesh.vbSize = m[0] * 84;
                    mesh.vbDepthOffset = m[3];
                    mesh.vbDepthSize = m[0] * 3 * layouts[(section - 1) * 2 + 1];
                    mesh.ibOffset = m[4];
                    mesh.ibSize = m[0] * 12;
                    mesh.draw[0] = { m[0] * 3, 0, 0 };
                    const auto* bounds = metadata + (bins + bin) * 6;
                    Math::Vector3 minimum(BoundsCoordinate(bounds[0]), BoundsCoordinate(bounds[1]), BoundsCoordinate(bounds[2]));
                    Math::Vector3 maximum(BoundsCoordinate(bounds[3]), BoundsCoordinate(bounds[4]), BoundsCoordinate(bounds[5]));
                    for (uint32_t axis = 0; axis < 3; ++axis)
                        valid &= std::isfinite(BoundsCoordinate(bounds[axis])) && std::isfinite(BoundsCoordinate(bounds[axis + 3]))
                            && BoundsCoordinate(bounds[axis]) <= BoundsCoordinate(bounds[axis + 3]);
                    low = Min(low, minimum);
                    high = Max(high, maximum);
                    const Math::Vector3 center = (minimum + maximum) * 0.5f;
                    XMStoreFloat4(reinterpret_cast<DirectX::XMFLOAT4*>(mesh.bounds), Math::Vector4(center, Length(maximum - center)));
                    valid &= m[0] == m[5] && m[2] % 28 == 0 && m[2] / 28 == m[1] * 3
                        && uint64_t(m[2]) + mesh.vbSize <= bytes && uint64_t(m[3]) + mesh.vbDepthSize <= bytes
                        && uint64_t(m[4]) + mesh.ibSize <= bytes && uint64_t(m[1]) + m[0] <= triangles
                        && (mesh.materialCBV == 0xffff || (uint64_t(mesh.materialCBV) + 1) * 256
                            <= output->Source->m_MaterialConstants.GetBufferSize());
                    result.Sections[body].push_back(uint32_t(output->Meshes.size()));
                    output->Meshes.push_back(mesh);
                    result.Triangles[body] += m[0];
                }
                sum += result.Triangles[body];
                if (!result.Triangles[body])
                {
                    // 빈 몸체는 유효한 빈 section 목록으로 보존하고 bounds를 출력하지 않는다.
                    PatchLog("MI4d empty body=%u tris=0 sections=0\n", body);
                    continue;
                }
                PatchLog("MI3c body=%u tris=%u sections=%zu bounds=(%g,%g,%g)-(%g,%g,%g)\n",
                    body, result.Triangles[body], result.Sections[body].size(), float(low.GetX()), float(low.GetY()),
                    float(low.GetZ()), float(high.GetX()), float(high.GetY()), float(high.GetZ()));
            }
            PatchLog("MI3c sum body=%llu input=%llu %s material/depth/IB/bounds=%s status=%u\n",
                sum, triangles, sum + job.Dropped == triangles ? "PASS" : "FAIL", valid ? "PASS" : "FAIL", flags);
            PatchLog("MI3f compact input=%llu dropped=%llu output=%llu\n", triangles, job.Dropped, sum);
            if (!valid || sum + job.Dropped != triangles) throw std::runtime_error("MI3c body compaction validation failed");
            m_BodyResult = std::move(result);
            job.Output.reset();
            m_BodyState = PatchBodyState::Done;

        }
        catch (const std::exception& error)
        {
            // 완료된 실패 출력만 버리고 호출자에게 실패 상태를 제공한다.
            if (m_BodyJob->VerifyClassify)
                Utility::Printf("Island GPU classify FAIL: readback=%s\n", error.what());
            m_BodyState = PatchBodyState::Failed;
            WritePatchLogSafely([&error]()
            {
                // 오류 문자열 생성과 파일 기록도 기존 게시 경로에 예외를 전파하지 않는다.
                return std::string("MI3c ERROR: ") + error.what() + "; existing A/B path preserved\n";
            });
        }
        m_BodyJob.reset();
    }

    // 분류 job을 이어받아 body×section 압축과 최종 회수 fence를 제출한다.
    void SkeletalPatchCopy::CompactCutBodies(const std::shared_ptr<const GeometryBuffers> (&inputs)[2], uint32_t bodyCount)
    {
        // 누락된 section도 원래 VB 주소로 대응시켜 재질과 cap을 구별한다.
        if (m_SkinPatch->Islands.TriangleSideBody.empty()) return;
        std::map<uint32_t, Mesh> templates;
        uint64_t triangles = 0, bytes = 0;
        for (const auto& input : inputs)
            for (const auto& mesh : input->Meshes)
            {
                if (mesh.vbStride != 28 || !mesh.vbSize || mesh.vbSize % 84
                    || mesh.ibFormat != DXGI_FORMAT_R32_UINT || mesh.ibSize != mesh.vbSize / 7)
                    throw std::runtime_error("MI3c unsupported cutter layout");
                const uint32_t depth = mesh.vbDepthSize / (mesh.vbSize / 28);
                if ((depth != 12 && depth != 16) || mesh.vbDepthSize % (mesh.vbSize / 28))
                    throw std::runtime_error("MI3c unsupported depth layout");
                const auto& other = templates.emplace(mesh.vbOffset, mesh).first->second;
                if (mesh.materialCBV != other.materialCBV || mesh.psoFlags != other.psoFlags
                    || depth != other.vbDepthSize / (other.vbSize / 28))
                    throw std::runtime_error("MI3c side section mismatch");
                triangles += mesh.vbSize / 84;
                bytes += uint64_t(mesh.vbSize / 28) * (28 + depth + 4);
            }

        // 라벨에서 사라진 몸체도 유지하며 잘못된 표면 라벨은 할당 전에 거절한다.
        if (!bodyCount || bodyCount >= UINT32_MAX - 1)
            throw std::length_error("MI4d invalid compaction body count");
        const uint32_t bodies = bodyCount;
        for (const auto& pair : m_SkinPatch->Islands.TriangleSideBody)
            for (uint32_t body : pair)
                if (body != UINT32_MAX && body >= bodies)
                    throw std::runtime_error("MI4d island body outside request groups");
        const uint32_t sections = uint32_t(templates.size());
        const uint64_t bins = uint64_t(bodies) * sections;
        if (!triangles || bytes > UINT32_MAX || triangles * 96 > UINT32_MAX || bins * 48 > UINT32_MAX)
            throw std::length_error("MI3c capacity exceeds RAW addressing");

        // 멤버 작업이 제출 입력과 부분 할당 출력의 수명을 소유한다.
        auto& job = *m_BodyJob;
        for (uint32_t side = 0; side < 2; ++side)
        {
            job.Inputs[side] = inputs[side];
            job.Bodies[side] = m_TriangleBodies[side].GetResource();
        }
        job.Templates = templates;
        job.BodyCount = bodies;
        job.Triangles = triangles;
        job.Bytes = bytes;
        std::vector<uint32_t> layouts;
        for (const auto& entry : templates)
        {
            layouts.push_back(28);
            layouts.push_back(entry.second.vbDepthSize / (entry.second.vbSize / 28));
        }
        auto output = job.Output = std::make_shared<GeometryBuffers>();
        job.LayoutData = layouts;
        output->Source = inputs[0]->Source;
        output->Parent = inputs[0]->Parent;
        output->Generation = inputs[0]->Generation;
        output->UncappedSegments = inputs[0]->UncappedSegments;
        const auto parent = output->Parent ? output->Parent->GetGenerated() : nullptr;
        output->LineageBytes = (parent ? parent->LineageBytes : 0) + bytes + triangles * 96 + 256;
        output->Data.Create(L"Body compact data", uint32_t(bytes / 4), 4);
        output->Provenance.Create(L"Body compact provenance", uint32_t(triangles * 24), 4);
        output->CapMaterial.Create(L"Body compact cap material", 64, 4);
        job.Layouts.Create(L"Body compact layouts", uint32_t(layouts.size()), 4);
        job.Bins.Create(L"Body compact bins and bounds", uint32_t(bins * 12), 4);
        job.Status.Create(L"Body compact status", 1, 4);
        job.Metadata.Create(L"Body compact metadata", uint32_t(bins * 12), 4);
        job.Flags.Create(L"Body compact flags", 1, 4);
        struct alignas(16) Constants
        {
            uint32_t Mode, Bodies, Sections, Section, FirstTriangle, Triangles, Vb, Depth;
            uint32_t Ib, Stride, DepthStride, CapacityBytes, CapacityTriangles, Bounds, Padding[2];
        } c{ 0, bodies, sections, 0, 0, 0, 0, 0, 0, 28, 0, uint32_t(bytes), uint32_t(triangles), 1, {} };

        // PSO를 재사용하고 count→GPU prefix→scatter 순서로 제출한다.
        static RootSignature root;
        static ComputePSO pso(L"Compact actual patch bodies");
        static bool initialized = false;
        if (!initialized)
        {
            root.Reset(9);
            root[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 4; ++i) root[i + 1].InitAsBufferSRV(i);
            for (uint32_t i = 0; i < 4; ++i) root[i + 5].InitAsBufferUAV(i);
            root.Finalize(L"Compact actual patch bodies");
            pso.SetRootSignature(root);
            pso.SetComputeShader(g_pPatchBodyCompactCS, sizeof(g_pPatchBodyCompactCS));
            pso.Finalize();
            initialized = true;
        }

        // 업로드 메모리는 같은 제출의 fence까지 유지되어 동기 대기가 없다.
        auto& graphics = GraphicsContext::Begin(L"Compact actual patch bodies");
        CutPerf::Gpu cutPerfCompact(graphics, "BodyCompact-whole");
        auto upload = graphics.ReserveUploadMemory(layouts.size() * sizeof(uint32_t));
        std::memcpy(upload.DataPtr, layouts.data(), layouts.size() * sizeof(uint32_t));
        graphics.CopyBufferRegion(job.Layouts, 0, upload.Buffer, upload.Offset, layouts.size() * sizeof(uint32_t));
        auto& compute = graphics.GetComputeContext();
        for (auto* buffer : { &output->Data, &output->Provenance, &job.Bins, &job.Status })
        {
            compute.TransitionResource(*buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.ClearUAV(*buffer);
            compute.InsertUAVBarrier(*buffer);
        }
        compute.TransitionResource(job.Layouts, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.SetRootSignature(root);
        compute.SetPipelineState(pso);
        compute.SetBufferSRV(4, job.Layouts);
        compute.SetBufferUAV(5, output->Data);
        compute.SetBufferUAV(6, output->Provenance);
        compute.SetBufferUAV(7, job.Bins);
        compute.SetBufferUAV(8, job.Status);
        for (c.Mode = 0; c.Mode < 3; ++c.Mode)
        {
            for (uint32_t side = 0; side < (c.Mode == 1 ? 1u : 2u); ++side)
            {
                auto& data = const_cast<ByteAddressBuffer&>(inputs[side]->Data);
                auto& provenance = const_cast<ByteAddressBuffer&>(inputs[side]->Provenance);
                for (auto* buffer : { &data, &provenance, &m_TriangleBodies[side] })
                    compute.TransitionResource(*buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                compute.SetBufferSRV(1, data);
                compute.SetBufferSRV(2, provenance);
                compute.SetBufferSRV(3, m_TriangleBodies[side]);
                for (const auto& mesh : inputs[side]->Meshes)
                {
                    c.Section = uint32_t(std::distance(templates.begin(), templates.find(mesh.vbOffset)));
                    c.FirstTriangle = mesh.vbOffset / 84; c.Triangles = mesh.vbSize / 84;
                    c.Vb = mesh.vbOffset; c.Depth = mesh.vbDepthOffset; c.Ib = mesh.ibOffset;
                    c.DepthStride = layouts[c.Section * 2 + 1];
                    compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
                    const uint32_t groups = (c.Triangles + 63) / 64;
                    compute.Dispatch(c.Mode == 1 ? 1 : std::min(groups, 65535u),
                        c.Mode == 1 ? 1 : (groups + 65534) / 65535);
                    if (c.Mode == 1) break;
                }
            }
            compute.InsertUAVBarrier(job.Bins);
            compute.InsertUAVBarrier(job.Status);
        }

        // 작은 표의 복사를 예약하고 입력과 출력을 읽기 상태로 돌려 비동기 제출한다.
        graphics.CopyBuffer(output->CapMaterial, const_cast<ByteAddressBuffer&>(inputs[0]->CapMaterial));
        graphics.CopyBuffer(job.Metadata, job.Bins);
        graphics.CopyBuffer(job.Flags, job.Status);
        for (const auto& input : inputs)
            for (auto* buffer : { &input->Data, &input->Provenance, &input->CapMaterial })
                graphics.TransitionResource(const_cast<ByteAddressBuffer&>(*buffer), D3D12_RESOURCE_STATE_GENERIC_READ);
        for (auto* buffer : { &output->Data, &output->Provenance, &output->CapMaterial })
            graphics.TransitionResource(*buffer, D3D12_RESOURCE_STATE_GENERIC_READ);
        cutPerfCompact.End();
        job.Fence = graphics.Finish(false);
        m_BodyState = PatchBodyState::InProgress;
    }

    // 복사본 결과를 먼저 소비하여 기존 Static 진단과 발행 집계에서 제외한다.
    bool SkeletalPatchCopy::OnCutResult(Scene& scene, const CuttingSystem& cutting, const CutResultEntry& result)
    {
        // 새 C로 삭제한 이전 복사본의 늦은 결과도 격리한다.
        for (auto it = m_RetiredOwners.begin(); it != m_RetiredOwners.end(); ++it)
            if (result.Owner == *it)
            {
                m_RetiredOwners.erase(it);
                return true;
            }
        if (!m_CopyOwner || result.Owner != m_CopyOwner) return false;
        if (!m_CutInFlight) return true;

        // 절단기가 직접 잰 GPU 구간(ms)과 요청부터 결과까지의 벽시계를 함께 남긴다.
        CutPerf::Mark("cut-result-received");
        CutPerf::Log("CUTTER id=%u gpu_ms=%.6f ready_ms=%.3f clip=%.6f boundary=%.6f cap=%.6f finalize=%.6f loops=%u status=%u\n",
            CutPerf::RequestId(), result.GpuMs, result.ReadyMs, result.StageMs[0], result.StageMs[1], result.StageMs[2],
            result.StageMs[3], result.Loops, unsigned(result.Status));

        // batch가 비었을 때 단독 제출했으므로 Last 진단은 이 결과에 대응한다.
        m_CutInFlight = false;
        // A는 양의 반공간이므로 B를 음의 법선으로 0.1 m 이동해 간격을 만든다.
        if (result.Status == CutResultStatus::Published)
        {
            // cutter side 0/1을 직접 사용하며 물리 A/B skin 순서로 뒤집지 않는다.
            if (m_SkinPatch)
            {
                auto* owner = scene.FindObject(m_CopyOwner);
                const ComponentId ids[] = { result.Mesh, result.Other };
                const std::shared_ptr<const GeometryBuffers> inputs[] = {
                    owner->FindMeshComponent(ids[0])->GetGeometry()->GetGenerated(),
                    owner->FindMeshComponent(ids[1])->GetGeometry()->GetGenerated() };
                CutPerf::Cpu cutPerfBodies("OnCutResult-classify-compact-submit");
                try
                {
                    // 분류나 압축 실패는 기존 A/B 게시 준비에 전파하지 않는다.
                    const auto real = RelabelCapLoops(result, *m_SkinPatch, m_Plane);
                    const auto capLoops = DiagnoseCapLoops(result, m_SkinPatch->Islands, real, uint32_t(m_BodySides.size()));
                    for (uint32_t side = 0; side < 2; ++side)
                        ClassifyCutBodies(*inputs[side], m_SkinPatch->Islands, capLoops, side, uint32_t(m_BodySides.size()));
                    CompactCutBodies(inputs, uint32_t(m_BodySides.size()));
                }
                catch (const std::exception& error)
                {
                    // 부분 할당 자원도 작업 수명 규칙으로 정리하고 실패를 기록한다.
                    DiscardBodyCompaction();
                    m_BodyState = PatchBodyState::Failed;
                    WritePatchLogSafely([&error]()
                    {
                        // 실패 로그도 예외를 격리하여 A/B 준비를 계속한다.
                        return std::string("MI3c ERROR: ") + error.what() + "; existing A/B path preserved\n";
                    });
                }
            }

            // 원본 게시 전 준비 owner의 두 조각은 모든 pass에서 계속 숨긴다.
            if (!m_OffsetCopy)
                for (const auto& piece : scene.FindObject(m_CopyOwner)->GetMeshComponents()) piece->PublishVisible = false;

            // 결과와 진단 수치를 보관하여 다음 Update에서 비동기 완료를 기다린다.
            m_ReadyCut = std::make_shared<CutResultEntry>(result);
            m_ReadyCaps = cutting.LastCaps;
            m_ReadyUncapped = cutting.LastUncapped;
            return true;
        }
        FinishCutResult(scene, result, cutting.LastCaps, cutting.LastUncapped);
        return true;
    }

    // 원본이 바뀐 미게시 절단은 break 없이 취소하고 숨긴 준비물을 정리한다.
    bool SkeletalPatchCopy::DiscardStaleCutResult(Scene& scene)
    {
        // offset 진단을 제외하고 Applied 이후와 같은 geometry·mask 기준으로 검사한다.
        if (m_OffsetCopy)
        {
            return false;
        }
        const auto* owner = scene.FindObject(m_Break.Owner);
        const auto* source = owner ? owner->FindMeshComponent(m_SourceMesh) : nullptr;
        if (source && source->GetGeometry() == m_SourceRevision
            && (source->GetTriangleMask() ? source->GetTriangleMask()->Revision : 0) == m_MaskRevision)
        {
            return false;
        }

        // 미게시 자원과 준비 상태를 먼저 폐기한 뒤 거절 완료를 기록한다.
        DiscardPreparedBodies(scene);
        PatchLog("MI3d discarded before break: source geometry/mask revision mismatch; breakSubmitted=0\n");
        LogCutResult(scene, "source revision mismatch before break", 0, 0, 0);
        return true;
    }

    // 숨긴 owner와 압축 자원을 해제하고 미게시 준비 상태를 초기화한다.
    void SkeletalPatchCopy::DiscardPreparedBodies(Scene& scene)
    {
        // component의 공유 geometry 참조를 먼저 없애 압축 출력까지 회수한다.
        if (scene.FindObject(m_CopyOwner))
        {
            scene.DeleteObject(m_CopyOwner);
        }
        m_CopyOwner = 0;
        DiscardBodyCompaction();

        // 이미 Update가 가져간 결과도 남은 준비 상태를 통해 재사용하지 못하게 한다.
        m_ReadyCut.reset();
        m_ReadyCaps = m_ReadyUncapped = 0;
        m_BreakReady = false;
        m_SkinPatch.reset();
        m_PreparedIds.clear();
        m_MultiPieces.clear();
    }

    // batch 제출 전에 전체 게시 용량과 조각 component 및 부착 값을 준비한다.
    void SkeletalPatchCopy::PrepareMultiPublication(Scene& scene)
    {
        // 원본 owner와 숨긴 몸체의 대응을 확인한다.
        auto* owner = scene.FindObject(m_Break.Owner);
        auto* staging = scene.FindObject(m_CopyOwner);
        if (!staging || m_PreparedIds.size() != m_Attachments.size())
        {
            throw std::runtime_error("MI4h2 missing prepared bodies");
        }

        // 빈 몸체를 제외한 전체 수를 첫 준비에서 한 번에 예약한다.
        const auto publishCount = size_t(std::count_if(m_PreparedIds.begin(), m_PreparedIds.end(), [](ComponentId id)
        {
            // 빈 몸체는 준비 ID가 없으므로 실제 게시 수에 포함하지 않는다.
            return id != InvalidComponentId;
        }));
        m_MultiPieces.reserve(publishCount);

        // 게시 전 조각의 변환과 부착 및 skin 활성화를 모두 끝낸다.
        for (size_t body = 0; body < m_PreparedIds.size(); ++body)
        {
            // 몸체 순서를 보존하면서 각자의 geometry와 Attachment 및 skin을 사용한다.
            if (m_PreparedIds[body] == InvalidComponentId)
            {
                continue;
            }
            const auto* prepared = staging->FindMeshComponent(m_PreparedIds[body]);
            auto piece = owner->PrepareMeshComponent(prepared->GetGeometry(), m_MultiPieces.empty() ? publishCount : 0);
            if (!piece->SetTransform(m_World, MeshTransformMode::WorldIndependent))
            {
                throw std::runtime_error("MI4h2 invalid piece world");
            }
            piece->SetInheritedCollision({}, piece->GetGeometry()->GetId(), "node attached");
            piece->Attachment = std::make_unique<NodeAttachment>(m_Attachments[body]);
            piece->PatchSkin = prepared->PatchSkin;
            piece->ActivatePatchSkin();
            piece->PublishVisible = true;
            m_PieceIds[body] = piece->GetId();
            m_MultiPieces.push_back(std::move(piece));
        }
    }

    // batch 전체 Applied 뒤 합집합 mask와 비어 있지 않은 몸체를 함께 게시한다.
    bool SkeletalPatchCopy::ConsumeMultiBreak(Scene& scene)
    {
        // pause 중에는 결과를 소비하지 않고 기존 요청 ID를 보존한다.
        std::vector<BreakStatus> statuses;
        if (!m_Physics->TryGetBreakResults(m_RequestId, statuses))
        {
            return false;
        }
        CutPerf::Mark("break-result");
        const uint64_t publishedRequest = m_RequestId;
        m_RequestId = 0;
        const bool applied = statuses.size() == m_MultiBreaks.size()
            && std::all_of(statuses.begin(), statuses.end(), [](BreakStatus status)
            {
                // 모든 요청의 commit 성공만 게시를 허용한다.
                return status == BreakStatus::Applied;
            });
        for (size_t i = 0; i < statuses.size(); ++i)
        {
            PatchLog("MI4h2 batch index=%zu BreakStatus=%u Applied=%u\n", i, unsigned(statuses[i]), statuses[i] == BreakStatus::Applied);
        }

        // batch 거절은 공통 폐기 후 원본과 constraint 상태가 그대로인지 검증한다.
        if (!applied)
        {
            const auto preparedOwner = m_CopyOwner;
            DiscardPreparedBodies(scene);
            const bool unchanged = m_MultiBefore == ReadOriginalState(scene, *m_Physics);
            const bool clean = !scene.FindObject(preparedOwner) && !m_CopyOwner
                && m_PreparedIds.empty() && !m_SkinPatch && !GetBodyResult();
            PatchLog("MI4h2 rejected original/mask/constraint=%s cleanup=%s publish=0 mask=0 constraintDelta=0\n",
                unchanged ? "PASS" : "FAIL", clean ? "PASS" : "FAIL");
            if (!unchanged || !clean)
            {
                throw std::runtime_error("MI4h2 batch rejection changed original or leaked preparation");
            }
            return true;
        }

        // 단일 경로와 같은 revision 규칙과 준비 ID로 게시 가능 여부를 확인한다.
        if (DiscardStaleCutResult(scene))
        {
            return true;
        }
        auto* owner = scene.FindObject(m_Break.Owner);
        auto* source = owner->FindMeshComponent(m_SourceMesh);

        // 합집합 전체를 mask 호출 한 번과 patch ID 하나로 숨긴다.
        if (!source->SetTriangleMask(m_Removal))
        {
            throw std::runtime_error("MI4h2 invalid removal mask");
        }
        const auto published = m_MultiPieces.size();
        for (auto& piece : m_MultiPieces)
        {
            owner->CommitMeshComponent(std::move(piece));
        }

        // 성공한 batch의 root별 이력을 기존 패치와 새 조각에 같은 규칙으로 적용한다.
        PublishDetachRecord(scene, *owner, *source, owner->FindMeshComponent(m_PieceIds.front()), m_PieceIds, publishedRequest);
        auto& context = GraphicsContext::Begin(L"Publish multi skeletal pieces");
        scene.PrepareMeshes(context);
        context.Finish();
        CutPerf::Mark("published");

        // 다중 경계 대응과 PS4 측정은 아직 지원하지 않음을 명시한다.
        PatchLog("MI4h2 boundary/PS4 measure N/A (multi)\n");

        // rig의 실제 끊긴 관절 수와 topology 증가를 게시 시점에 기록한다.
        SkeletalRigState state;
        auto& skeletal = m_Physics->GetSkeletalPhysics();
        skeletal.ReadDiagnosticState(m_Break.Owner, state);
        LogStaleSkin(*owner, state, publishedRequest);
        size_t constraints = 0;
        for (uint32_t joint = 0; joint < state.Joints.size(); ++joint)
        {
            constraints += skeletal.GetConstraintHandle(m_Break.Owner, joint) != MiniPhysics::InvalidConstraint;
        }
        std::set<uint32_t> patchIds;
        for (const auto value : source->GetTriangleMask()->Pending)
        {
            if (value & 0x80000000u)
            {
                patchIds.insert(value & 65535u);
            }
        }
        const auto topologyDelta = skeletal.GetTopologyGeneration(m_Break.Owner) - m_Break.ExpectedTopologyGeneration;
        PatchLog("MI4h2 published pieces=%zu maskCalls=1 patchIds=%zu topologyDelta=%llu constraints=%zu->%zu\n",
            published, patchIds.size(), topologyDelta, m_MultiConstraints, constraints);
        if (topologyDelta != m_MultiBreaks.size() || m_MultiConstraints - constraints != m_MultiBreaks.size())
        {
            throw std::runtime_error("MI4h2 unexpected rig delta");
        }

        // 기존 demo 캡처가 게시 프레임과 지정 간격의 후속 프레임을 수집하게 한다.
        m_DemoPublished = true;
        m_CaptureRequest = m_Demo;
        DiscardPreparedBodies(scene);
        return true;
    }

    // n+1 압축 몸체의 숨긴 component와 skin을 검증하고 원본 상태 보존을 확인한다.
    void SkeletalPatchCopy::PrepareMultiBodies(Scene& scene)
    {
        // 강제 revision 불일치에서도 원본을 건드리지 않고 기존 거절 경로를 검증한다.
        uint32_t stale = 0;
        CommandLineArgs::GetInteger(L"skeletal-multi-stale", stale);
        const auto before = ReadOriginalState(scene, *m_Physics);
        if (stale)
        {
            ++m_MaskRevision;
        }
        if (DiscardStaleCutResult(scene))
        {
            const bool unchanged = before == ReadOriginalState(scene, *m_Physics);
            PatchLog("MI4h1 rejected revision original/mask/constraint=%s publish=0 break=0 mask=0\n",
                unchanged ? "PASS" : "FAIL");
            if (!unchanged)
            {
                throw std::runtime_error("MI4h1 rejected request changed original");
            }
            return;
        }

        // 몸체 수와 snapshot 수가 일치할 때 압축 section의 독립 준비물을 만든다.
        const auto* compacted = GetBodyResult();
        auto* staging = scene.FindObject(m_CopyOwner);
        if (!compacted || !staging || compacted->Triangles.size() != m_Attachments.size()
            || m_SkinSnapshots.size() != m_Attachments.size() || !m_SkinPatch)
        {
            throw std::runtime_error("MI4h1 body preparation input mismatch");
        }
        const auto revision = GeometryRevision::FromGenerated(compacted->Geometry);
        for (uint32_t body = 0; body < compacted->Triangles.size(); ++body)
        {
            // 빈 몸체는 준비 대상에서 제외하되 몸체 배열의 자리는 유지한다.
            if (!compacted->Triangles[body])
            {
                PatchLog("MI4h1 body=%u tris=0 empty skipped\n", body);
                continue;
            }
            auto& piece = staging->AddMeshComponent(revision->SelectSections(compacted->Sections[body]));
            piece.PublishVisible = false;
            m_PreparedIds[body] = piece.GetId();

            // 명시 실패 진단은 첫 몸체 준비 후 두 번째 snapshot의 skin을 거절한다.
            uint32_t fail = 0;
            CommandLineArgs::GetInteger(L"skeletal-multi-fail", fail);
            if (fail && body == 1)
            {
                m_SkinSnapshots[body].SkinningAllowed = false;
            }
            BuildPieceSkin(piece, *compacted->Geometry, *m_SkinPatch, body);
            const bool ready = piece.PatchSkin && piece.PatchSkin->VertexCount == compacted->Triangles[body] * 3;
            PatchLog("MI4h1 body=%u tris=%u attach=%u frozen=%zu skin=%s visible=0\n",
                body, compacted->Triangles[body], m_Attachments[body].NodeId,
                size_t(std::count(m_SkinSnapshots[body].Frozen.begin(), m_SkinSnapshots[body].Frozen.end(), uint8_t(1))),
                ready ? "PASS" : "FAIL");
            if (!ready)
            {
                throw std::runtime_error("MI4h1 body skin preparation failed");
            }
        }

        // h1은 break와 mask를 제출하지 않고 준비 전후 원본 상태를 대조한다.
        const bool unchanged = before == ReadOriginalState(scene, *m_Physics);
        PatchLog("MI4h1 prepared bodies=%zu original/mask/constraint=%s publish=0 break=0 mask=0\n",
            m_Attachments.size(), unchanged ? "PASS" : "FAIL");
        if (!unchanged)
        {
            throw std::runtime_error("MI4h1 preparation changed original");
        }
    }

    // 요청 몸체 수의 준비 ID와 skin을 확보한 뒤 단일 또는 batch break를 제출한다.
    void SkeletalPatchCopy::FinishCutResult(Scene& scene, const CutResultEntry& result, uint32_t caps, uint32_t uncapped)
    {
        // 성능 로그에 마무리 단계 시작 시각과 CPU 구간을 남긴다.
        CutPerf::Mark("finish-start");
        CutPerf::Cpu cutPerfFinish("FinishCutResult");

        // 지연 처리에서도 동일한 절단 기록으로 성공과 실패 이유를 복원한다.
        const char* reason = result.Status == CutResultStatus::Published ? "published A/B" :
            result.Reason ? result.Reason : result.Error & 1 ? "finite rectangle endpoints outside" :
            result.Error ? "cutter fatal error; original copy kept" : "no split; original copy kept";
        if (result.Status == CutResultStatus::Published)
        {
            // body 0/1은 A/B이며 cutter side 순서와 독립이다.
            auto* staging = scene.FindObject(m_CopyOwner);

            // 다중 몸체는 숨긴 skin 준비 뒤 batch 제출 또는 기존 합집합 진단으로 나눈다.
            if (m_BodySides.size() != 2)
            {
                // 준비 중 어느 예외가 발생해도 완료 기록 전에 부분 생성물을 폐기한다.
                const auto preparedOwner = m_CopyOwner;
                std::vector<uint64_t> original;
                try
                {
                    m_PreparedIds.assign(m_BodySides.size(), InvalidComponentId);
                    if (m_MultiPrepare)
                    {
                        original = ReadOriginalState(scene, *m_Physics);
                        PrepareMultiBodies(scene);
                        if (!m_CopyOwner)
                        {
                            return;
                        }
                        PrepareMultiPublication(scene);
                    }
                    if (!GetBodyResult())
                    {
                        throw std::runtime_error("MI4d2 union compaction failed");
                    }
                }
                catch (const std::exception& error)
                {
                    // 실패 원인을 기록하기 전에 owner와 압축 자원 및 준비 상태를 폐기한다.
                    DiscardPreparedBodies(scene);
                    LogCutResult(scene, "multi body preparation failed", caps, uncapped, 1);

                    // 명시 진단은 주입한 skin 실패에 한해 정리 완료를 검사하고 소비한다.
                    uint32_t fail = 0;
                    CommandLineArgs::GetInteger(L"skeletal-multi-fail", fail);
                    if (fail && std::strcmp(error.what(), "MI4h1 body skin preparation failed") == 0)
                    {
                        const bool clean = !scene.FindObject(preparedOwner) && !m_CopyOwner
                            && !m_BodyJob && !GetBodyResult() && m_BodyState == PatchBodyState::None
                            && m_PreparedIds.empty() && !m_SkinPatch && !m_BreakReady
                            && !m_ReadyCut && !m_ReadyCaps && !m_ReadyUncapped
                            && original == ReadOriginalState(scene, *m_Physics);
                        PatchLog("MI4h1 forced failure cleanup=%s owner=0 components=0 compact=0 ids=0 skin=0 breakReady=0 original/mask/constraint=unchanged\n",
                            clean ? "PASS" : "FAIL");
                        if (clean)
                        {
                            return;
                        }
                    }
                    throw;
                }
                catch (...)
                {
                    // 표준 예외 이외의 실패도 같은 정리와 완료 기록을 보장한다.
                    DiscardPreparedBodies(scene);
                    LogCutResult(scene, "multi body preparation failed", caps, uncapped, 1);
                    throw;
                }

                // 모든 skin 준비와 원본 revision 검사가 끝난 뒤 batch를 한 번 제출한다.
                if (m_MultiPrepare)
                {
                    if (DiscardStaleCutResult(scene))
                    {
                        return;
                    }
                    m_MultiBefore = ReadOriginalState(scene, *m_Physics);

                    // 실제 runtime constraint handle 수를 제출 전 기준으로 보관한다.
                    SkeletalRigState rig;
                    auto& skeletal = m_Physics->GetSkeletalPhysics();
                    skeletal.ReadDiagnosticState(m_Break.Owner, rig);
                    m_MultiConstraints = 0;
                    for (uint32_t joint = 0; joint < rig.Joints.size(); ++joint)
                    {
                        m_MultiConstraints += skeletal.GetConstraintHandle(m_Break.Owner, joint) != MiniPhysics::InvalidConstraint;
                    }

                    // 기본 off인 거절 진단은 마지막 요청의 세대만 틀리게 한다.
                    auto requests = m_MultiBreaks;
                    uint32_t reject = 0;
                    CommandLineArgs::GetInteger(L"skeletal-multi-reject", reject);
                    if (reject)
                    {
                        ++requests.back().ExpectedTopologyGeneration;
                    }
                    m_RequestId = m_Physics->QueueBreaks(requests, true);
                    m_BreakReady = false;
                    PatchLog("MI4h2 queued RequestId=%llu count=%zu %s\n", m_RequestId,
                        requests.size(), m_Physics->IsSimulationRunning() ? "" : "paused: waiting");
                    LogCutResult(scene, "multi bodies awaiting batch", caps, uncapped, result.Error);
                    return;
                }

                // 성공 진단도 자원과 준비 상태를 먼저 폐기한 뒤 완료를 기록한다.
                DiscardPreparedBodies(scene);
                PatchLog("MI4d2 union diagnostic bodies=%zu compaction=PASS publish=0 break=0 mask=0\n",
                    m_BodySides.size());
                LogCutResult(scene, "MI4d2 union diagnostic; publish skipped", caps, uncapped, result.Error);
                return;
            }

            // 기존 단일 break의 두 준비물은 요청의 반공간 순서로 대응한다.
            m_PreparedIds.assign(m_BodySides.size(), InvalidComponentId);
            for (size_t body = 0; body < m_BodySides.size(); ++body)
            {
                // 두 side 출력에서 몸체의 기존 폴백 반공간을 선택한다.
                m_PreparedIds[body] = m_BodySides[body] == 0 ? result.Mesh : result.Other;
            }
            const auto* compacted = GetBodyResult();

            // 전체 side 다수결과 무관하게 검증된 두 몸체의 압축 결과를 게시한다.
            const char* fallbackReason = !compacted ? "compaction unavailable/failed"
                : compacted->Triangles.size() != 2 ? "invalid compacted body count"
                : !compacted->Triangles[0] || !compacted->Triangles[1] ? "empty compacted body" : nullptr;
            const bool fallback = fallbackReason != nullptr;
            if (!m_OffsetCopy && !fallback)
            {
                const auto revision = GeometryRevision::FromGenerated(compacted->Geometry);
                for (uint32_t body = 0; body < 2; ++body)
                    if (!staging->FindMeshComponent(m_PreparedIds[body])->SetGeometry(
                        revision->SelectSections(compacted->Sections[body])))
                        throw std::runtime_error("MI3d body geometry rejected");
            }

            // 실제 게시할 삼각형 수와 기존 side 대체 여부를 한 번 기록한다.
            if (!m_OffsetCopy)
            {
                uint32_t triangles[2]{};
                for (uint32_t body = 0; body < 2; ++body)
                {
                    const auto& revision = *staging->FindMeshComponent(m_PreparedIds[body])->GetGeometry();
                    for (const auto& section : revision.GetSections())
                        triangles[body] += revision.GetGenerated()->Meshes[section.SourceMesh].ibSize / 12;
                }
                PatchLog("MI3d publish body0 tris=%u body1 tris=%u fallback=%u\n", triangles[0], triangles[1], fallback);
                if (fallback)
                {
                    PatchLog("MI3d fallback: %s; existing side results\n", fallbackReason);
                }
            }
            // cutter 출력과 추출 출처가 살아 있는 동안 A/B skin 입력을 동기로 생성한다.
            if (m_SkinPatch && m_SkinSnapshots.size() == m_BodySides.size())
            {
                auto* owner = scene.FindObject(m_CopyOwner);
                for (uint32_t body = 0; body < 2; ++body)
                {
                    auto* piece = owner->FindMeshComponent(m_PreparedIds[body]);
                    if (piece && m_SkinSnapshots[body].SkinningAllowed)
                        BuildPieceSkin(*piece, *piece->GetGeometry()->GetGenerated(), *m_SkinPatch, body);
                }
            }
            m_SkinPatch.reset();

            // A/B 준비 성공 뒤에만 저장된 C 세대로 무충격 break를 요청한다.
            if (m_BreakReady)
            {
                SkeletalRigState before;
                m_Physics->GetSkeletalPhysics().ReadDiagnosticState(m_Break.Owner, before);
                m_DynamicBefore = 0;
                for (const auto& body : before.Bodies) m_DynamicBefore += body.Motion == MiniPhysics::BodyMotion::Dynamic;
                // 진단에서만 저장된 topology 세대를 바꿔 실제 Stale 거절 경로를 통과시킨다.
                auto request = m_Break;
                if (m_ForceStale) ++request.ExpectedTopologyGeneration;

                // 압축 후 skin 준비가 끝난 시점에도 원본이 같을 때만 관절을 끊는다.
                if (DiscardStaleCutResult(scene)) return;
                m_RequestId = m_Physics->QueueBreak(request, true);
                PatchLog("SP7b queued RequestId=%llu JointId=%u ParentNode=%u %s\n", m_RequestId,
                    m_Break.JointId, m_Incoming.ParentNode, m_Physics->IsSimulationRunning() ? "" : "paused: waiting");
                m_BreakReady = false;
            }

            if (!m_OffsetCopy)
            {
                // C snapshot의 외곽 정점을 숨긴 A/B의 baked 정점과 대응시킨다.
                if (!m_ForceStale && m_BreakCheck)
                {
                    const auto* prepared = scene.FindObject(m_CopyOwner);
                    const MeshComponent* pieces[] = { prepared->FindMeshComponent(m_PreparedIds[0]), prepared->FindMeshComponent(m_PreparedIds[1]) };

                    // 짝 누락도 게시·after60 측정에서 FAIL로 집계하도록 대응 결과를 보존한다.
                    PairBoundary(pieces, m_Boundary, m_BoundaryPairs);
                    if (pieces[0])
                        CheckBoundaryFlags(*pieces[0]);
                }
                LogCutResult(scene, reason, caps, uncapped, result.Error);
                return;
            }
            auto* owner = scene.FindObject(m_CopyOwner);
            auto* piece = owner ? owner->FindMeshComponent(result.Other) : nullptr;
            SceneTransform world;
            if (!piece || !SceneTransform::TryFromAffine(piece->GetWorldTransform(owner->GetTransform()), world))
                throw std::runtime_error("Skeletal patch B world transform invalid");
            const Math::Vector3 before = world.GetTranslation();
            const Math::Vector3 delta = -Math::Normalize(Math::Vector3(m_Plane.WorldPlane)) * 0.1f;
            world.SetTranslation(world.GetTranslation() + delta);
            if (!piece->SetTransform(world, MeshTransformMode::WorldIndependent))
                throw std::runtime_error("Skeletal patch B gap transform rejected");
            auto& context = GraphicsContext::Begin(L"Prepare separated skeletal patch B");
            piece->Update(context, *owner->GetModelInstance(), owner->GetTransform());
            context.Finish();
            // 게시된 실제 월드 TRS의 변화를 읽어 단위와 변환 모드 적용을 확인한다.
            const Math::Vector3 actual = piece->GetWorldTransform(owner->GetTransform()).GetTranslation() - before;
            PatchLog("Skeletal patch B gap world=%.6f delta=(%.6f,%.6f,%.6f)\n",
                float(Math::Length(actual)), float(actual.GetX()), float(actual.GetY()), float(actual.GetZ()));
        }
        else
        {
            // 실패한 절단은 더 이상 추출 출처를 사용하지 않는다.
            m_SkinPatch.reset();
            PatchLog("Skeletal patch B gap not applied: %s\n", reason);
        }
        LogCutResult(scene, reason, caps, uncapped, result.Error);

        // 절단 자체가 실패하면 숨긴 준비물도 즉시 버린다.
        if (!m_OffsetCopy && result.Status != CutResultStatus::Published)
        {
            scene.DeleteObject(m_CopyOwner);
            m_CopyOwner = 0;
            m_BreakReady = false;
        }
        return;
    }

    // 열린 패치의 surface 게시 또는 거절을 컴포넌트 수와 함께 기록한다.
    void SkeletalPatchCopy::LogCutResult(const Scene& scene, const char* reason, uint32_t caps, uint32_t uncapped, uint32_t error)
    {
        // cap 수를 강제하지 않고 성공과 명시적 거절을 모두 최종 결과로 남긴다.
        const auto* owner = scene.FindObject(m_CopyOwner);
        const size_t components = owner ? owner->GetMeshComponents().size() : 0;
        PatchLog("Skeletal patch cut result: %s components=%zu caps=%u uncapped=%u error=%u\n",
            reason, components, caps, uncapped, error);

        // 선택 수와 게시 결과를 CPU 비교 진단에 기록한다.
        if (m_Diagnostic)
            PatchLog("SP6 comparison case=%u model=%s mode=%s NodeId=%u name=%s patch=%u GPU==CPU=PASS components/caps/uncapped/error=%zu/%u/%u/%u\n",
                m_DiagnosticCase, m_ModelName.c_str(), m_Mode == SkeletalJointMode::Rig ? "Rig" : m_Mode == SkeletalJointMode::Parent ? "Parent" : "Influence", m_SelectedNode,
                DiagnosticNodeName(m_ModelName, m_SelectedNode).c_str(), m_Result.PatchTriangles, components, caps, uncapped, error);
        m_Completed = true;
        if (m_Diagnostic && components != 2 && (!reason || !*reason || std::strcmp(reason, "published A/B") == 0))
            throw std::runtime_error("Skeletal patch cut diagnostic missing rejection reason");
    }

    // 모든 skin 모델에서 기존 세 평면과 두 blend-zone 진단 평면을 만든다.
    bool BuildSkeletalPatchDiagnosticPlane(const Scene& scene, uint32_t index, MeshCutting::CutPlaneData& plane)
    {
        // 기본 장면 순서의 skin 모델마다 다섯 case를 배정하고 생성 복사본은 제외한다.
        using namespace Math;
        uint32_t modelIndex = 0;
        for (const auto& object : scene.GetObjects())
        {
            const auto* pose = object->GetModelInstance();
            if (!pose || !pose->HasValidPose()) continue;
            const auto& model = *pose->GetModel();
            if (!model.m_NumJoints || !object->IsCuttable()) continue;
            if (modelIndex++ != index / 5) continue;
            const auto& component = *object->GetMeshComponents().front();
            const auto& parents = pose->GetPoseParents();
            std::vector<bool> skin(model.m_NumNodes, false), hasChild(model.m_NumNodes, false);
            std::vector<uint32_t> parent(model.m_NumNodes, UINT32_MAX), depth(model.m_NumNodes, 0);
            std::vector<Vector3> positions(model.m_NumNodes, Vector3(kZero));

            // 각 내부 Mesh의 팔레트와 mesh node 변환으로 월드 joint 위치를 모은다.
            for (size_t section = 0; section < component.GetGeometry()->GetSections().size(); ++section)
            {
                const auto& mesh = component.GetGeometry()->GetMesh(section);
                const auto world = JointFrameToWorld(*pose, component, object->GetTransform(), section);
                for (uint32_t j = 0; j < mesh.numJoints; ++j)
                {
                    const uint32_t node = model.m_JointIndices[mesh.startJoint + j];
                    skin[node] = model.m_InfluencingJoints[node] != 0;
                    positions[node] = Vector3(world * Vector4(Vector3(pose->GetNodeTransforms()[node].GetW()), 1));
                }
            }
            for (uint32_t node = 0; node < model.m_NumNodes; ++node)
            {
                if (!skin[node]) continue;
                uint32_t ancestor = parents[node];
                while (ancestor != UINT32_MAX && !skin[ancestor]) ancestor = parents[ancestor];
                parent[node] = ancestor;
                if (ancestor != UINT32_MAX) hasChild[ancestor] = true;
                for (uint32_t p = ancestor; p != UINT32_MAX; p = parents[p]) ++depth[node];
            }

            // 충분한 깊이의 긴 segment 두 개와 가장 긴 말단 segment를 결정적으로 고른다.
            std::vector<uint32_t> segments, leaves;
            for (uint32_t node = 0; node < model.m_NumNodes; ++node)
                if (skin[node] && parent[node] != UINT32_MAX && depth[node] >= 2
                    && float(Length(positions[node] - positions[parent[node]])) > 1e-5f)
                {
                    segments.push_back(node);
                    if (!hasChild[node]) leaves.push_back(node);
                }

            // 같은 길이는 NodeId로 정렬하여 모델 내부 단위와 이름에 의존하지 않는다.
            const auto longer = [&](uint32_t a, uint32_t b)
            {
                // 길이는 월드 좌표에서 비교한다.
                const float la = LengthSquare(positions[a] - positions[parent[a]]);
                const float lb = LengthSquare(positions[b] - positions[parent[b]]);
                return la != lb ? la > lb : a < b;
            };
            std::sort(segments.begin(), segments.end(), longer);
            std::sort(leaves.begin(), leaves.end(), longer);
            if (segments.size() < 2 || leaves.empty()) return false;
            const uint32_t region = index % 5;
            const bool leaf = region == 2;
            // blend-zone은 최장 segment의 바로 위 segment로 골라 관절 주변을 비교한다.
            const uint32_t blendChild = parent[segments.front()];
            if (parent[blendChild] == UINT32_MAX) return false;
            const uint32_t child = leaf ? leaves.front() : region < 2 ? segments[region] : blendChild;
            const Vector3 a = positions[parent[child]], b = positions[child];
            Vector3 normal = Normalize(b - a);
            const Vector3 x = Normalize(Cross(normal, std::abs(float(normal.GetY())) < 0.9f ? Vector3(kYUnitVector) : Vector3(kXUnitVector)));
            const Vector3 origin = leaf ? b : region == 3 ? a + (b - a) * 0.9f : (a + b) * 0.5f;

            // 이름 없이 같은 최장 segment의 중점에 45도 사선 평면을 만든다.
            if (region == 4) normal = Normalize(normal + x);
            const Vector3 planeX = region == 4 ? Normalize(Cross(normal, Cross(x, Normalize(b - a)))) : x;
            const float size = float(component.GetBoundingSphere(object->GetTransform()).GetRadius()) * 0.15f;
            PatchLog("Skeletal patch case=%u model=%s expected=%s parent=%u child=%u rectangle=%.6f\n",
                index, object->GetName().c_str(), leaf ? "leaf" : region == 3 ? "near-child-90pct" : region == 4 ? "oblique-45deg" : "segment", parent[child], child, size);
            return MeshCutting::TryBuildCutPlaneData({ size, size }, AffineTransform(Matrix3(planeX, normal, Cross(planeX, normal)), origin), plane);
        }
        return false;
    }
    // 완료 전 작업 자원을 파괴하지 않도록 제출 fence를 기다린다.
    SkeletalPatchJob::~SkeletalPatchJob()
    {
        // 작업 자원은 마지막 제출 fence 뒤에 해제한다.
        Cancel();
    }

    // 이전 제출을 무효화하고 GPU가 사용하는 임시 자원의 수명을 보장한다.
    void SkeletalPatchJob::Cancel()
    {
        // 새 요청에서 자원을 재사용하기 전에 이전 GPU 사용을 마친다.
        if (m_Fence) Graphics::g_CommandManager.WaitForFence(m_Fence);
        m_Fence = 0;
        m_Output.reset();
        m_Input.reset();
        m_PoseOffsets.clear();
        m_Parents.clear();
        m_IslandJoints.clear();
        m_IslandNodes.clear();
        m_IslandFrames.clear();
        m_SkinSections.clear();
        m_IslandLabels = {};
        m_ClassifyGpuReady = false;
    }

    // 컴포넌트 제거 상태를 반영해 추출과 GPU 섬 번호·투표를 같은 제출에 기록한다.
    bool SkeletalPatchJob::Submit(const MeshComponent& source, const MeshCutting::CutPlaneData& plane,
        const std::vector<std::vector<uint32_t>>& mask, const Math::Matrix4& world,
        uint32_t node, bool diagnostic, bool influence, const std::vector<NodeId>& parents, uint32_t child,
        const std::vector<uint32_t>& unionRoots, const std::vector<uint32_t>& unionChildren)
    {
        // 준비되지 않았거나 생성된 입력은 이 단계의 skeletal 추출 대상이 아니다.
        Cancel();
        const auto& input = *source.GetGeometry();
        const auto& posed = source.GetPosedGeometry();
        if (!Renderer::UseComputePose || !posed.IsReady() || input.IsGenerated()
            || (!influence && unionRoots.empty() && mask.size() != input.GetSections().size())) return false;

        // 명시한 P 집합은 B_i 하위 팔레트 합집합으로 바꾸고 기존 Selected 판정을 재사용한다.
        std::vector<std::vector<uint32_t>> palettes;
        for (uint32_t section = 0; section < input.GetSections().size(); ++section)
        {
            const auto& part = input.GetMesh(section);
            palettes.emplace_back(input.GetSource()->m_JointIndices.get() + part.startJoint,
                input.GetSource()->m_JointIndices.get() + part.startJoint + part.numJoints);
        }
        if (!unionRoots.empty() && influence)
            return false;
        m_SelectionMasks = unionRoots.empty() ? mask : BuildJointUnionMasks(parents, palettes, unionRoots);
        if (!influence && m_SelectionMasks.size() != palettes.size())
            return false;

        // 각 원본 draw를 한 번만 등록해 겹치는 B_i도 같은 prefix 슬롯을 사용한다.
        InitializePatch();
        std::vector<DrawInput> draws;
        std::vector<uint32_t> masks;
        uint64_t triangles = 0;
        // 정점 선택 비트 버퍼에 필요한 총 word 수를 section별로 누적한다.
        uint64_t bitWords = 0;
        // skin section별 첫 draw 번호와 정점 수를 모아 Mode 8 dispatch 입력으로 쓴다.
        std::vector<std::pair<uint32_t, uint32_t>> bitSections;
        // skin section의 첫 draw 번호와 정점 수
        for (uint32_t s = 0; s < input.GetSections().size(); ++s)
        {
            const auto& mesh = input.GetMesh(s);
            if (!influence && m_SelectionMasks[s].size() != mesh.numJoints)
                return false;
            const uint32_t maskOffset = uint32_t(masks.size());
            if (influence)
                masks.insert(masks.end(), input.GetSource()->m_JointIndices.get() + mesh.startJoint,
                    input.GetSource()->m_JointIndices.get() + mesh.startJoint + mesh.numJoints);
            else masks.insert(masks.end(), m_SelectionMasks[s].begin(), m_SelectionMasks[s].end());
            m_PoseOffsets.push_back(posed.GetOffset(input.GetSections()[s].SourceMesh));

            // section의 정점 선택 비트는 32비트 word 경계에서 시작한다.
            const uint32_t wordOffset = uint32_t(bitWords);
            const size_t drawsBefore = draws.size();
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                const auto& draw = mesh.draw[d];
                if (!draw.primCount)
                    continue;
                draws.push_back({ mesh.vbOffset, mesh.vbStride, mesh.ibOffset, mesh.ibFormat == DXGI_FORMAT_R32_UINT,
                    draw.startIndex, draw.baseVertex, draw.primCount / 3, m_PoseOffsets.back(),
                    uint32_t(triangles), maskOffset, mesh.psoFlags, s, d, mesh.numJoints, mesh.vbSize / mesh.vbStride, wordOffset });
                triangles += draw.primCount / 3;
            }

            // draw가 있는 skin section만 정점 수만큼 비트 word를 확보한다.
            if ((mesh.psoFlags & PSOFlags::kHasSkin) && draws.size() > drawsBefore)
            {
                bitSections.push_back({ uint32_t(drawsBefore), mesh.vbSize / mesh.vbStride });
                bitWords += (uint64_t(mesh.vbSize / mesh.vbStride) + 31) / 32;
            }
        }
        if (!triangles || triangles > UINT32_MAX / (3 * 48ull) || bitWords > UINT32_MAX / 64
            || !input.GetSource()->m_NumNodes || input.GetSource()->m_NumNodes > UINT32_MAX / 8) return false;

        // 대상 전체 삼각형 수를 상한으로 잡아 compaction 출력 overflow를 없앤다.
        m_Input = source.GetGeometry();
        m_Plane = plane;
        m_Node = node;
        m_Diagnostic = diagnostic;
        m_Influence = influence;
        m_SourceWorld = world;
        // GPU가 사용한 준비 팔레트와 node/source/world 합성을 제출 시점에 고정한다.
        const auto& model = *input.GetSource();
        m_Parents = parents;
        m_CutRoots = unionRoots.empty() ? std::vector<uint32_t>{node} : unionRoots;
        if (!unionChildren.empty() && unionChildren.size() != m_CutRoots.size())
        {
            throw std::invalid_argument("MI4d2 child snapshot count mismatch");
        }
        m_CutChildren = unionChildren.empty() ? std::vector<uint32_t>(m_CutRoots.size(), UINT32_MAX) : unionChildren;
        m_UnionSubmit = !unionRoots.empty();
        if (!m_UnionSubmit)
        {
            m_CutChildren[0] = child;
        }
        m_IslandJoints.assign(source.GetPreparedJoints(), source.GetPreparedJoints() + model.m_NumJoints);
        m_IslandNodes.assign(source.GetPreparedNodes(), source.GetPreparedNodes() + model.m_NumNodes);
        const uint8_t* meshData = model.m_MeshData.get();
        std::vector<uint32_t> skinSections(model.m_NumMeshes, UINT32_MAX);
        uint32_t skin = 0;
        for (uint32_t s = 0; s < model.m_NumMeshes; ++s)
        {
            const auto& mesh = *reinterpret_cast<const Mesh*>(meshData);
            if (mesh.numJoints)
                skinSections[s] = skin++;
            meshData += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
        }
        for (uint32_t s = 0; s < input.GetSections().size(); ++s)
        {
            m_SkinSections.push_back(skinSections.at(input.GetSections()[s].SourceMesh));
            m_IslandFrames.push_back(Invert(Math::Matrix4(source.GetDeformBinding().SourceFrame.ToAffine()))
                * source.GetPreparedNodes()[input.GetMesh(s).meshCBV]);
        }
        m_Output = std::make_shared<SkeletalPatchBuffers>();
        m_Output->Geometry = std::make_shared<GeometryBuffers>();
        auto& geometry = *m_Output->Geometry;
        geometry.Source = input.GetSource();
        const uint32_t vertices = uint32_t(triangles) * 3;
        Parameters c{};
        c.Plane = plane.WorldPlane;
        c.LocalToRectangle = plane.WorldToPlane;
        c.LocalToWorld = world;
        c.Epsilon = 1e-5f;
        c.HalfWidth = plane.HalfWidth;
        c.HalfHeight = plane.HalfHeight;
        c.NodeCount = input.GetSource()->m_NumNodes;
        c.Influence = influence;
        const auto* triangleMask = source.GetTriangleMask();
        c.HasState = triangleMask != nullptr;
        m_State = triangleMask ? triangleMask->Pending : std::vector<uint32_t>{};
        if (PatchLengthWeightsMode())
            m_Output->PreviousState = m_State;
        c.Triangles = uint32_t(triangles);
        c.Draws = uint32_t(draws.size());
        c.DepthOffset = vertices * 28;
        c.IndexOffset = vertices * 44;
        geometry.Data.Create(L"Patch packed geometry", vertices * 48, 1);
        geometry.LineageBytes = uint64_t(vertices) * 48;
        Mesh mesh = input.GetMesh(draws.front().SectionId);
        mesh.vbOffset = 0;
        mesh.vbStride = 28;
        mesh.vbSize = vertices * 28;
        mesh.vbDepthOffset = c.DepthOffset;
        mesh.vbDepthSize = vertices * ((mesh.psoFlags & 64) ? 16 : 12);
        mesh.ibOffset = c.IndexOffset;
        mesh.ibSize = vertices * 4;
        mesh.ibFormat = DXGI_FORMAT_R32_UINT;
        mesh.meshCBV = 0;
        mesh.numJoints = 0;
        mesh.startJoint = 0;
        mesh.numDraws = 1;
        mesh.psoFlags = (mesh.psoFlags & ~PSOFlags::kHasSkin) | 31;
        mesh.pso = Renderer::GetPSO(mesh.psoFlags);
        mesh.draw[0] = { 0, 0, 0 };
        geometry.Meshes.push_back(mesh);
        alignas(16) const uint32_t metadata[] = { 0, 0, 0, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0, 0, 0, UINT32_MAX, 0, 0 };
        m_Output->LocalBounds.Create(L"Patch counts and local bounds", 11, 4, metadata);
        m_Output->TriangleSources.Create(L"Patch triangle sources", uint32_t(triangles), 12);

        // 기본 경로는 유지하고 GPU 옵션에서만 거리와 셀 버퍼를 만든다.
        uint32_t islandGpu = 0, islandVerify = 0;
        CommandLineArgs::GetInteger(L"island-gpu", islandGpu);
        CommandLineArgs::GetInteger(L"island-gpu-verify", islandVerify);
        m_IslandVerify = islandGpu != 0 || islandVerify != 0;
        m_IslandGpu = islandGpu || m_IslandVerify;
        m_ConsumeGpu = islandGpu != 0;
        m_ClassifyGpuReady = false;
        m_IslandConverged = false;
        m_VoteSupported = node != UINT32_MAX && !influence;

        // 64스레드 거리·UF와 256스레드 번호 부여의 최대 그룹 수를 함께 검사한다.
        const uint64_t largestCapacity = std::max(triangles, uint64_t(model.m_LatticeTopology.CellCount) * 2);
        m_IslandDispatchLimited = m_IslandGpu
            && (largestCapacity + 63) / 64 > D3D12_CS_DISPATCH_MAX_THREAD_GROUPS_PER_DIMENSION;
        if (m_IslandDispatchLimited)
        {
            m_IslandGpu = false;
            m_IslandVerify = false;
        }

        // 셰이더 word·바이트 주소를 64비트로 검사하고 버퍼당 256 MiB로 제한한다.
        const uint64_t keys64 = triangles * 2;
        const uint64_t groups64 = uint64_t(m_CutRoots.size()) + 1;
        const uint64_t words64 = keys64 * (groups64 + 5) + keys64 + 3;
        const uint64_t weightWords = triangles * 24;
        if (m_IslandGpu && (words64 > UINT32_MAX / 4 || words64 * 4 > 256ull * 1024 * 1024
            || weightWords > UINT32_MAX / 4 || weightWords * 4 > 256ull * 1024 * 1024))
        {
            m_IslandGpu = false;
            m_IslandVerify = false;
            Utility::Printf("Island GPU UF fallback reason=vote-size\n");
        }

        // 선택적 준비 중 던져진 예외는 추출과 CPU 라벨링을 중단하지 않는다.
        std::vector<uint32_t> weights;
        try
        {
            if (m_IslandGpu)
            {
                // 활성 요청에서만 거리 파이프라인과 월드 평면을 준비한다.
                InitializeDistance();
                m_IslandK = 32;
                CommandLineArgs::GetInteger(L"island-gpu-k", m_IslandK);
                const uint32_t nodes = model.m_LatticeTopology.CellCount * 2;
                m_IslandParent.Create(L"Island parents", nodes, 4);
                m_IslandStatus.Create(L"Island status", 2, 4);
                m_StatusReadback.Create(L"Island status readback", 2, 4);

                // 번호 부여 임시 배열과 최종 표는 요청이 소유한다.
                m_IslandRootMin.Create(L"Island minimum keys", nodes, 4);
                const uint32_t keys = uint32_t(triangles) * 2;
                m_IslandPrefix.Create(L"Island head prefix", keys + (keys + 255) / 256, 4);
                m_TriangleSideIsland.Create(L"Triangle side islands", uint32_t(triangles), 8);
                m_NumberReadback.Create(L"Island number mirror", uint32_t(triangles), 8);

                // oracle과 동일한 bind weight와 그룹을 원본 draw 순서로 올린다.
                const std::vector<uint32_t> jointNodes(model.m_JointIndices.get(), model.m_JointIndices.get() + model.m_NumJoints);
                const auto bodyGroups = BuildCutBodyGroups(parents, jointNodes, m_CutRoots);
                weights.assign(size_t(weightWords), 0);
                for (const auto& draw : draws)
                {
                    const uint32_t skin = m_SkinSections[draw.SectionId];
                    if (skin == UINT32_MAX)
                        continue;
                    const auto& section = model.m_SkinCpuSections[skin];
                    uint32_t local = 0;
                    for (uint32_t d = 0; d < draw.DrawId; ++d)
                        local += input.GetMesh(draw.SectionId).draw[d].primCount / 3;
                    for (uint32_t t = 0; t < draw.Triangles; ++t)
                    {
                        for (uint32_t k = 0; k < 3; ++k)
                        {
                            const auto& vertex = section.at(model.m_LatticeTopology.Indices[skin].at((local + t) * 3 + k));
                            const size_t at = (size_t(draw.FirstTriangle + t) * 3 + k) * 8;
                            for (uint32_t j = 0; j < 4; ++j)
                            {
                                weights[at + j] = vertex.Weights[j];
                                weights[at + 4 + j] = !vertex.Weights[j] || bodyGroups.NodeGroups.empty() ? 0 : bodyGroups.NodeGroups.at(jointNodes.at(section.StartJoint + vertex.Joints[j]));
                            }
                        }
                    }
                }
                m_VoteGroups = uint32_t(m_CutRoots.size()) + 1;
                m_VoteCapacity = uint32_t(triangles);
                const uint32_t words = uint32_t(words64);
                m_VoteWeights.Create(L"Island bind weights and groups", uint32_t(weights.size()), 4);
                m_IslandVote.Create(L"Island vote output", words, 4);
                m_VoteReadback.Create(L"Island vote mirror", words, 4);
                if (m_IslandVerify)
                    m_ParentReadback.Create(L"Island parent readback", nodes, 4);
                m_DistancePlane = Transpose(Invert(world)) * plane.WorldPlane;

                // GPU 코너 출력과 선택적 검증 회수 버퍼를 만든다.
                m_CornerDistance.Create(L"Island corner distances", uint32_t(triangles), 12);
                m_CornerCell.Create(L"Island corner cells", uint32_t(triangles), 12);
                if (m_IslandVerify)
                {
                    m_DistanceReadback.Create(L"Island distance verification", uint32_t(triangles), 12);
                    m_CellReadback.Create(L"Island cell verification", uint32_t(triangles), 12);
                }
            }

        }
        catch (const std::exception& error)
        {
            // 부분 생성된 선택적 버퍼를 해제해 필수 추출 할당에 메모리를 돌려준다.
            m_CornerDistance.Destroy();
            m_CornerCell.Destroy();
            m_IslandParent.Destroy();
            m_IslandStatus.Destroy();
            m_StatusReadback.Destroy();
            m_IslandRootMin.Destroy();
            m_IslandPrefix.Destroy();
            m_TriangleSideIsland.Destroy();
            m_NumberReadback.Destroy();
            m_VoteWeights.Destroy();
            m_IslandVote.Destroy();
            m_VoteReadback.Destroy();
            m_ParentReadback.Destroy();
            m_DistanceReadback.Destroy();
            m_CellReadback.Destroy();

            // 이번 요청의 모든 섬 패스를 생략하고 CPU 라벨링을 유지한다.
            m_IslandGpu = false;
            m_IslandVerify = false;
            Utility::Printf("Island GPU UF fallback reason=gpu-prepare %s\n", error.what());
        }

        // 삼각형 prefix 뒤에 블록별 합계와 offset을 위한 16바이트 영역을 확보한다.
        const uint32_t blocks = uint32_t((triangles + 63) / 64);
        m_Scan.Create(L"Patch inclusive prefix", uint32_t(triangles + 2 * blocks), 8);

        // draw와 팔레트 입력 및 선택 결과 버퍼를 준비한다.
        m_Descriptors.Create(L"Patch draw inputs", uint32_t(draws.size()), sizeof(DrawInput), draws.data());
        if (masks.empty())
            masks.push_back(0);
        m_Masks.Create(L"Patch palette mask", uint32_t(masks.size()), 4, masks.data());
        m_Readback.Create(L"Patch counts and bounds readback", 12, 4);
        const std::vector<uint32_t> votes(size_t(c.NodeCount) * 2, 0);
        m_Votes.Create(L"Patch uint64 joint votes", c.NodeCount, 8, votes.data());
        alignas(16) const uint32_t selectedNode[4] = { node, 0, 0, 0 };
        m_Selected.Create(L"Patch selected NodeId", 1, 4, selectedNode);

        // 정점 선택 비트 버퍼는 job이 재사용하고 모자랄 때만 이전 자원을 fence까지 보존한 뒤 다시 만든다.
        if (!m_Bits.GetResource() || bitWords > m_BitCapacity)
        {
            if (m_Bits.GetResource())
                Graphics::g_CommandManager.RetireResource(m_Bits.GetResource());
            m_BitCapacity = uint32_t(std::max<uint64_t>(bitWords, 1));
            m_Bits.Create(L"Patch vertex selection bits", m_BitCapacity, 4);
        }

        // 마스크가 없으면 State SRV에 바인딩할 0 버퍼를 준비한다.
        const std::vector<uint32_t> emptyState(triangleMask ? 4 : (size_t(triangles) + 3) & ~size_t(3), 0);
        m_EmptyState.Create(L"Empty triangle state", uint32_t(emptyState.size()), 4, emptyState.data());

        // 같은 graphics queue에서 pose 준비 뒤 추출하고 패스 사이 UAV 의존성을 보장한다.
        auto& graphics = GraphicsContext::Begin(L"Extract skeletal patch");
        CutPerf::Gpu cutPerfExtract(graphics, "Extract-whole");
        auto& compute = graphics.GetComputeContext();

        // G1a와 같이 제출 context의 업로드 메모리로 weight를 비동기 복사한다.
        if (m_IslandGpu)
        {
            try
            {
                // 업로드 페이지 할당 실패는 엔진 공통 ASSERT_SUCCEEDED 규약을 따른다.
                // 이 catch는 선택적 업로드 준비 중 던져진 예외만 처리한다.
                DynAlloc upload = graphics.ReserveUploadMemory(weights.size() * sizeof(uint32_t));
                std::memcpy(upload.DataPtr, weights.data(), weights.size() * sizeof(uint32_t));
                graphics.TransitionResource(m_VoteWeights, D3D12_RESOURCE_STATE_COPY_DEST);
                graphics.CopyBufferRegion(m_VoteWeights, 0, upload.Buffer, upload.Offset, weights.size() * sizeof(uint32_t));
                graphics.TransitionResource(m_VoteWeights, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            catch (const std::exception& error)
            {
                // 아직 섬 패스를 기록하지 않은 선택적 버퍼를 해제한다.
                m_CornerDistance.Destroy();
                m_CornerCell.Destroy();
                m_IslandParent.Destroy();
                m_IslandStatus.Destroy();
                m_StatusReadback.Destroy();
                m_IslandRootMin.Destroy();
                m_IslandPrefix.Destroy();
                m_TriangleSideIsland.Destroy();
                m_NumberReadback.Destroy();
                m_VoteWeights.Destroy();
                m_IslandVote.Destroy();
                m_VoteReadback.Destroy();
                m_ParentReadback.Destroy();
                m_DistanceReadback.Destroy();
                m_CellReadback.Destroy();

                // 추출 command list는 계속 사용하고 CPU 라벨링으로 처리한다.
                m_IslandGpu = false;
                m_IslandVerify = false;
                Utility::Printf("Island GPU UF fallback reason=gpu-prepare %s\n", error.what());
            }
        }

        if (triangleMask)
            const_cast<MeshComponent&>(source).UpdateTriangleMask(graphics);
        compute.SetRootSignature(PatchRoot);
        compute.SetPipelineState(PatchPSO);
        auto& inputData = const_cast<ByteAddressBuffer&>(input.GetDataBuffer());
        auto& inputPose = const_cast<ByteAddressBuffer&>(posed.GetBuffer());
        compute.TransitionResource(inputData, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(inputPose, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(m_Descriptors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(m_Masks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.SetBufferSRV(1, input.GetDataBuffer());
        compute.SetBufferSRV(2, posed.GetBuffer());
        compute.SetBufferSRV(3, m_Descriptors);
        compute.SetBufferSRV(4, m_Masks);
        auto& state = triangleMask ? const_cast<StructuredBuffer&>(triangleMask->State) : m_EmptyState;
        compute.TransitionResource(state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.SetBufferSRV(11, state);
        GpuBuffer* outputs[] = { &geometry.Data, &m_Output->TriangleSources, &m_Output->LocalBounds, &m_Scan, &m_Votes, &m_Selected };
        for (uint32_t i = 0; i < 6; ++i)
        {
            compute.TransitionResource(*outputs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.SetBufferUAV(i + 5, *outputs[i]);
        }
        compute.TransitionResource(m_Bits, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.SetBufferUAV(12, m_Bits);
        const uint32_t groups = blocks;

        // GPU 선택 결과를 CPU 대기 없이 추출에 연결한다.
        if (influence)
        {
            c.Mode = 3;
            compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
            compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
            compute.InsertUAVBarrier(m_Votes);
            c.Mode = 4;
            compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
            compute.Dispatch(1);
            compute.InsertUAVBarrier(m_Selected);
        }

        // skin section마다 정점 선택을 한 번씩만 판정해 비트로 모으고 추출 패스가 읽기 전에 완료를 보장한다.
        if (!bitSections.empty())
        {
            c.Mode = 8;
            for (const auto& section : bitSections)
            {
                c.BitDraw = section.first;
                compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
                const uint32_t bitGroups = (section.second + 63) / 64;
                compute.Dispatch(std::min(bitGroups, 65535u), (bitGroups + 65534) / 65535);
            }
            compute.InsertUAVBarrier(m_Bits);
        }

        // 블록 prefix와 블록 offset을 계산하고 추출할 때 두 값을 더한다.
        const uint32_t modes[] = { 0, 1, 6, 2 };
        for (uint32_t mode : modes)
        {
            // 블록 합계 scan만 한 그룹을 쓰고 나머지는 기존 2D 분할을 유지한다.
            c.Mode = mode;
            compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
            {
                // 모드별 dispatch 시간을 따로 잰다. 이름은 Mode 번호를 따른다.
                const char* modeNames[] = { "Extract-mode0", "Extract-mode1", "Extract-mode2", "", "", "", "Extract-mode6" };
                CutPerf::Gpu cutPerfMode(compute, modeNames[c.Mode]);
                if (c.Mode == 6)
                    compute.Dispatch(1);
                else compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
            }

            // 다음 패스가 읽을 scan과 기존 bounds 초기화의 UAV 의존성을 보장한다.
            if (c.Mode != 2)
            {
                compute.InsertUAVBarrier(m_Scan);
                compute.InsertUAVBarrier(m_Output->LocalBounds);
            }
        }

        // PG가 덮이기 전에 추출 출처와 카운터의 UAV 쓰기를 완료하고 거리를 계산한다.
        if (m_IslandGpu)
        {
            compute.InsertUAVBarrier(m_Output->TriangleSources);
            compute.InsertUAVBarrier(m_Output->LocalBounds);
            compute.TransitionResource(m_Output->TriangleSources, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            compute.TransitionResource(m_Output->LocalBounds, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            compute.TransitionResource(m_CornerDistance, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.TransitionResource(m_CornerCell, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.SetRootSignature(DistanceRoot);
            compute.SetPipelineState(DistancePSO);
            compute.SetBufferSRV(1, m_Output->TriangleSources);
            compute.SetBufferSRV(2, m_Output->LocalBounds);
            compute.SetBufferSRV(3, m_Descriptors);
            compute.SetBufferSRV(4, inputData);
            compute.SetBufferSRV(5, inputPose);
            compute.SetBufferUAV(7, m_CornerDistance);
            compute.SetBufferUAV(8, m_CornerCell);
            DistanceParameters distance{ m_DistancePlane, world, 0, c.Draws, c.Triangles, 0 };
            for (uint32_t section = 0; section < m_SkinSections.size(); ++section)
            {
                // section별 출력 슬롯은 겹치지 않으며 CPU와 같은 skin 번호를 쓴다.
                const uint32_t skinSection = m_SkinSections[section];
                if (skinSection == UINT32_MAX)
                    continue;
                auto& cells = const_cast<StructuredBuffer&>(model.m_GpuLatticeTopology.SectionVertexCell.at(skinSection));
                compute.TransitionResource(cells, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                compute.SetBufferSRV(6, cells);
                distance.Section = section;
                compute.SetDynamicConstantBufferView(0, sizeof(distance), &distance);
                compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
            }

            // 검증 전용 복사도 추출 fence에 포함하고 일반 GPU 옵션에서는 회수하지 않는다.
            if (m_IslandVerify)
            {
                graphics.CopyBuffer(m_DistanceReadback, m_CornerDistance);
                graphics.CopyBuffer(m_CellReadback, m_CornerCell);
            }
            compute.TransitionResource(m_CornerDistance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            compute.TransitionResource(m_CornerCell, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

            // 같은 제출에서 초기화 뒤 K번 hook과 jump를 기록한다.
            compute.TransitionResource(m_IslandParent, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.TransitionResource(m_IslandStatus, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.SetRootSignature(UnionRoot);
            compute.SetBufferSRV(1, m_Output->LocalBounds);
            compute.SetBufferSRV(2, m_CornerDistance);
            compute.SetBufferSRV(3, m_CornerCell);
            compute.SetBufferUAV(4, m_IslandParent);
            compute.SetBufferUAV(5, m_IslandStatus);
            const uint32_t nodes = model.m_LatticeTopology.CellCount * 2, nodeGroups = (nodes + 63) / 64;
            compute.SetConstants(0, c.Triangles, nodes, 1u, 0u);
            compute.SetPipelineState(JumpPSO);
            compute.Dispatch(std::min(nodeGroups, 65535u), (nodeGroups + 65534) / 65535);
            compute.InsertUAVBarrier(m_IslandParent);
            compute.InsertUAVBarrier(m_IslandStatus);
            compute.SetConstants(0, c.Triangles, nodes, 0u, 0u);
            for (uint32_t iteration = 0; iteration < m_IslandK; ++iteration)
            {
                compute.SetPipelineState(HookPSO);
                compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
                compute.InsertUAVBarrier(m_IslandParent);
                compute.SetPipelineState(JumpPSO);
                compute.Dispatch(std::min(nodeGroups, 65535u), (nodeGroups + 65534) / 65535);
                compute.InsertUAVBarrier(m_IslandParent);
            }

            // 검증 상태와 선택적 부모 회수를 기존 추출 fence에 포함한다.
            compute.SetPipelineState(VerifyPSO);
            compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
            compute.InsertUAVBarrier(m_IslandStatus);

            // 수렴 상태에서만 최소 키·두 단계 scan·최종 번호 기록을 실행한다.
            compute.TransitionResource(m_IslandRootMin, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.TransitionResource(m_IslandPrefix, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.TransitionResource(m_TriangleSideIsland, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.SetBufferUAV(6, m_IslandRootMin);
            compute.SetBufferUAV(7, m_IslandPrefix);
            compute.SetBufferUAV(8, m_TriangleSideIsland);
            compute.SetPipelineState(NumberPSO);
            for (uint32_t pass = 0; pass < 5; ++pass)
            {
                // 초기화는 노드 범위, scan은 키 범위, 최종 기록은 삼각형 범위를 쓴다.
                const uint32_t count = pass == 0 ? nodes : pass == 3 ? 1 : pass == 4 ? c.Triangles : c.Triangles * 2;
                compute.SetConstants(0, c.Triangles, nodes, pass, 0u);
                compute.Dispatch((count + 255) / 256);
                compute.InsertUAVBarrier(m_IslandRootMin);
                compute.InsertUAVBarrier(m_IslandPrefix);
                compute.InsertUAVBarrier(m_TriangleSideIsland);
                compute.InsertUAVBarrier(m_IslandStatus);
            }
            // 수렴한 번호로 정수 투표와 최종 표를 같은 fence에 기록한다.
            graphics.CopyBuffer(m_NumberReadback, m_TriangleSideIsland);
            compute.TransitionResource(m_TriangleSideIsland, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            compute.TransitionResource(m_VoteWeights, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            compute.TransitionResource(m_IslandVote, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.SetRootSignature(VoteRoot);
            compute.SetPipelineState(VotePSO);
            compute.SetBufferSRV(1, m_Output->LocalBounds);
            compute.SetBufferSRV(2, m_Output->TriangleSources);
            compute.SetBufferSRV(3, m_Descriptors);
            compute.SetBufferSRV(4, m_VoteWeights);
            compute.SetBufferSRV(5, m_TriangleSideIsland);
            compute.SetBufferUAV(6, m_IslandStatus);
            compute.SetBufferUAV(7, m_IslandVote);
            for (uint32_t pass = 0; pass < 3; ++pass)
            {
                compute.SetConstants(0, c.Triangles, m_VoteGroups, pass, c.Draws);
                compute.Dispatch(pass == 1 ? (c.Triangles + 63) / 64 : 1);
                compute.InsertUAVBarrier(m_IslandVote);
            }
            graphics.CopyBuffer(m_VoteReadback, m_IslandVote);
            graphics.CopyBuffer(m_StatusReadback, m_IslandStatus);
            if (m_IslandVerify)
                graphics.CopyBuffer(m_ParentReadback, m_IslandParent);
        }

        // 추출 카운터와 선택 본을 같은 제출의 readback 버퍼에 복사한다.
        graphics.TransitionResource(m_Output->LocalBounds, D3D12_RESOURCE_STATE_COPY_SOURCE);
        graphics.CopyBufferRegion(m_Readback, 0, m_Output->LocalBounds, 0, 44);
        graphics.TransitionResource(m_Selected, D3D12_RESOURCE_STATE_COPY_SOURCE);
        graphics.CopyBufferRegion(m_Readback, 44, m_Selected, 0, 4);

        // 원본 숨김에 필요한 compact 출처는 모든 제출에서 보관한다.
        m_OriginsReadback.Create(L"Patch sources", uint32_t(triangles), 12);
        graphics.CopyBuffer(m_OriginsReadback, m_Output->TriangleSources);

        // 진단에서만 동일 제출 시점의 VB/IB·pose를 저장한다.
        if (diagnostic)
        {
            m_SourceReadback.Create(L"Patch diagnostic source", uint32_t(input.GetDataBuffer().GetBufferSize()), 1);
            m_PoseReadback.Create(L"Patch diagnostic pose", uint32_t(posed.GetBuffer().GetBufferSize()), 1);
            graphics.CopyBuffer(m_SourceReadback, inputData);
            graphics.CopyBuffer(m_PoseReadback, inputPose);
            m_OutputReadback.Create(L"Patch diagnostic packed vertices", vertices * 48, 1);
            graphics.CopyBuffer(m_OutputReadback, geometry.Data);
        }
        graphics.TransitionResource(inputData, D3D12_RESOURCE_STATE_GENERIC_READ);
        graphics.TransitionResource(inputPose, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        graphics.TransitionResource(geometry.Data, D3D12_RESOURCE_STATE_GENERIC_READ);
        graphics.TransitionResource(m_Output->TriangleSources, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        graphics.TransitionResource(m_Output->LocalBounds, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        cutPerfExtract.End();
        m_Fence = graphics.Finish();
        return true;
    }

    // fence 완료 뒤 카운터·경계와 GPU 섬 수렴 상태·개수를 수집한다.
    bool SkeletalPatchJob::Poll(SkeletalPatchResult& result, std::shared_ptr<SkeletalPatchBuffers>& patch)
    {
        // GPU가 아직 쓰는 readback은 매핑하지 않는다.
        if (!m_Fence || !Graphics::g_CommandManager.IsFenceComplete(m_Fence))
            return false;

        // 디스패치 한계로 생략한 요청도 quiet와 무관하게 CPU fallback 사유를 남긴다.
        if (m_IslandDispatchLimited)
            Utility::Printf("Island GPU UF fallback reason=dispatch-limit\n");

        // 기존 fence 완료 뒤 UF 상태를 읽고 출력은 계속 CPU 경로로 계산한다.
        if (m_IslandGpu)
        {
            // 회수 실패 때 이전 요청의 수렴 상태를 재사용하지 않는다.
            m_IslandConverged = false;
            try
            {
                // 성공한 매핑은 범위를 벗어날 때 해제한다.
                const auto unmap = [](ReadbackBuffer* buffer)
                {
                    // 상태 회수 버퍼의 읽기 매핑을 해제한다.
                    buffer->Unmap();
                };
                const auto* status = static_cast<const uint32_t*>(m_StatusReadback.Map());
                const std::unique_ptr<ReadbackBuffer, decltype(unmap)> statusMap(&m_StatusReadback, unmap);
                m_IslandConverged = status[0] == 0;
                m_GpuIslandCount = m_IslandConverged ? status[1] : 0;
                const uint32_t error = status[0];
                if (error)
                    Utility::Printf("Island GPU UF fallback reason=not-converged/verify-failed iterations=%u\n", m_IslandK);
                else Utility::Printf("Island GPU UF converged iterations=%u\n", m_IslandK);
            }
            catch (const std::exception& error)
            {
                Utility::Printf("Island GPU UF fallback reason=status-map %s\n", error.what());
            }
        }

        // 기본 경로는 정점 대신 선택 수와 로컬 경계·원본 월드 X 경계만 읽는다.
        uint32_t metadata[12];
        std::memcpy(metadata, m_Readback.Map(), sizeof(metadata));
        std::memcpy(&result, metadata, sizeof(result));
        m_Node = metadata[11];

        // 단일 제출은 GPU가 고른 NodeId(Influence 포함)를 P_0로 확정한다.
        if (!m_UnionSubmit)
        {
            m_CutRoots[0] = m_Node;
        }
        PatchLog("Skeletal patch GPU selected mode=%s NodeId=%u\n", m_Influence ? "Influence" : "Parent", m_Node);
        m_Readback.Unmap();
        m_Output->SourceWorldWidth = BoundsCoordinate(metadata[10]) - BoundsCoordinate(metadata[9]);
        PatchLog("Skeletal patch source posed world X=[%.6f,%.6f] width=%.6f offset=%.6f\n",
            BoundsCoordinate(metadata[9]), BoundsCoordinate(metadata[10]), m_Output->SourceWorldWidth,
            m_Output->SourceWorldWidth + 0.5f);
        PatchLog("Skeletal patch GPU patch=%u\n", result.PatchTriangles);

        // section과 draw 출처를 원본 mask의 누적 삼각형 번호로 변환한다.
        const auto* origins = static_cast<const uint32_t*>(m_OriginsReadback.Map());
        for (uint32_t t = 0; t < result.PatchTriangles; ++t)
        {
            uint32_t first = 0;
            for (uint32_t s = 0; s <= origins[t * 3]; ++s)
                for (uint32_t d = 0; d < m_Input->GetMesh(s).numDraws; ++d)
                    if (s < origins[t * 3] || d < origins[t * 3 + 1])
                        first += m_Input->GetMesh(s).draw[d].primCount / 3;
            m_Output->Triangles.push_back(first + origins[t * 3 + 2]);
            // draw별 번호를 MI-1이 보존한 skin section 연속 삼각형 번호로 바꾼다.
            uint32_t local = origins[t * 3 + 2];
            for (uint32_t d = 0; d < origins[t * 3 + 1]; ++d)
                local += m_Input->GetMesh(origins[t * 3]).draw[d].primCount / 3;
            m_Output->IslandSources.push_back({ m_SkinSections.at(origins[t * 3]), local });
        }
        m_OriginsReadback.Unmap();
        if (m_Diagnostic)
            Validate(result);
        auto& mesh = m_Output->Geometry->Meshes[0];

        // bind pose의 원본 구 대신 현재 추출 정점의 로컬 경계 구를 설정한다.
        Math::Vector3 center(Math::kZero);
        float radius = 0;
        if (result.PatchTriangles)
        {
            const Math::Vector3 minimum(BoundsCoordinate(metadata[3]), BoundsCoordinate(metadata[4]), BoundsCoordinate(metadata[5]));
            const Math::Vector3 maximum(BoundsCoordinate(metadata[6]), BoundsCoordinate(metadata[7]), BoundsCoordinate(metadata[8]));
            center = (minimum + maximum) * 0.5f;
            radius = Math::Length(maximum - minimum) * 0.5f;
        }
        DirectX::XMStoreFloat4(reinterpret_cast<DirectX::XMFLOAT4*>(mesh.bounds), Math::Vector4(center, radius));

        // 압축된 실제 개수로 정적 draw의 유효 구간을 줄인다.
        const uint32_t vertices = result.PatchTriangles * 3;
        mesh.vbSize = vertices * 28;
        mesh.ibSize = vertices * 4;
        mesh.vbDepthSize = vertices * ((mesh.psoFlags & PSOFlags::kAlphaTest) ? 16 : 12);
        mesh.draw[0].primCount = vertices;

        // 절단 진단 옵션에서만 압축된 논리 출력과 실제 생성한 출력 버퍼 할당량을 기록한다.
        uint32_t validate = 0;
        if (CommandLineArgs::GetInteger(L"cut-validate", validate) && validate != 0)
        {
            uint64_t allocated = 0;
            GpuBuffer* buffers[] = { &m_Output->Geometry->Data, &m_Output->TriangleSources, &m_Output->LocalBounds };
            for (auto* buffer : buffers)
            {
                const auto desc = buffer->GetResource()->GetDesc();
                allocated += Graphics::g_Device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
            }
            Utility::Printf("Cut output bytes: vertices=%u logicalPerVertex=52 logicalTotal=%llu allocated=%llu\n",
                vertices, static_cast<unsigned long long>(uint64_t(vertices) * 52),
                static_cast<unsigned long long>(allocated));
        }

        // 수집한 출력의 소유권을 넘기고 다음 요청이 사용할 fence 상태를 비운다.
        patch = std::move(m_Output);
        m_Fence = 0;
        return true;
    }

    // CPU oracle과 GPU mirror를 비교하고 선택한 섬 표를 cap 보정에 전달한다.
    void SkeletalPatchJob::DiagnoseIslands(SkeletalPatchBuffers& patch, const MeshCutting::CutPlaneData& plane,
        float offset, bool positiveA, const std::string& modelName)
    {
        // 함수 전체 CPU 시간을 기존 성능 로그 옵션에서만 기록한다.
        CutPerf::Cpu cutPerfCpu("DiagnoseIslands");

        // skin section별 제출 프레임을 찾고 아직 계산하지 않은 부호를 표시한다.
        // 정점 재스키닝과 평면 부호 계산 시간을 따로 잰다.
        CutPerf::Cpu cutPerfSigns("DiagnoseIslands-reskin-sign");
        const auto& model = *m_Input->GetSource();
        std::vector<std::vector<int8_t>> signs(model.m_SkinCpuSections.size());
        std::vector<std::vector<float>> cpuDistances(m_IslandVerify ? signs.size() : 0);
        std::vector<uint32_t> frames(signs.size(), UINT32_MAX);
        for (size_t s = 0; s < m_SkinSections.size(); ++s)
        {
            const uint32_t skin = m_SkinSections[s];
            if (skin == UINT32_MAX)
                continue;
            frames.at(skin) = uint32_t(s);
        }

        // 패치 삼각형이 참조하는 정점만 중복 없이 PreparePoseCS와 같은 방식으로 스키닝한다.
        for (const auto& source : patch.IslandSources)
        {
            const uint32_t skin = source[0];
            const auto& section = model.m_SkinCpuSections.at(skin);
            if (signs[skin].empty())
                signs[skin].resize(section.size(), 2);
            if (m_IslandVerify && cpuDistances[skin].empty())
                cpuDistances[skin].resize(section.size());
            for (uint32_t k = 0; k < 3; ++k)
            {
                const uint32_t v = model.m_LatticeTopology.Indices.at(skin).at(source[1] * 3 + k);
                if (signs[skin][v] != 2)
                    continue;
                const auto& vertex = section.at(v);
                Math::Vector4 skinned(Math::kZero);
                uint32_t sum = 0;
                for (uint32_t weight : vertex.Weights)
                    sum += weight;
                for (uint32_t j = 0; j < 4; ++j)
                    if (vertex.Weights[j])
                        skinned = skinned + (m_IslandJoints.at(section.StartJoint + vertex.Joints[j]).posXform
                            * Math::Vector4(vertex.Position[0], vertex.Position[1], vertex.Position[2], 1))
                            * (float(vertex.Weights[j]) / std::max(sum, 1u));
                const Math::Vector3 local(m_IslandFrames.at(frames.at(skin)) * skinned);
                const Math::Vector3 world(m_SourceWorld * Math::Vector4(local, 1));
                const float distance = Math::Dot(plane.WorldPlane,
                    Math::Vector4(world + Math::Vector3(offset, 0, 0), 1));
                signs[skin][v] = std::abs(distance) <= 1e-5f ? 0 : distance > 0 ? 1 : -1;
                if (m_IslandVerify)
                    cpuDistances[skin][v] = distance;
            }
        }

        // 모든 비교가 성공해야 동일 번호의 CPU 점수와 GPU 표를 함께 소비한다.
        bool distanceMatch = false;
        bool partitionMatch = false;
        bool numberMatch = false;

        // Poll의 fence 완료 뒤 실제 이동 평면으로 코너별 부호와 셀을 대조한다.
        if (m_IslandVerify)
        {
            // 두 Map과 비교 중 예외가 나도 성공한 매핑을 범위 종료 시 해제한다.
            try
            {
                // 버퍼 소유권을 유지하며 읽기 매핑만 해제한다.
                const auto unmap = [](ReadbackBuffer* buffer)
                {
                    // 범위를 벗어난 회수 버퍼의 매핑을 해제한다.
                    buffer->Unmap();
                };
                const auto* distances = static_cast<const float*>(m_DistanceReadback.Map());
                const std::unique_ptr<ReadbackBuffer, decltype(unmap)> distanceMap(&m_DistanceReadback, unmap);
                const auto* cells = static_cast<const uint32_t*>(m_CellReadback.Map());
                const std::unique_ptr<ReadbackBuffer, decltype(unmap)> cellMap(&m_CellReadback, unmap);

                // 실제 이동 평면의 거리와 셀을 CPU 기준과 비교한다.
                uint32_t signMismatch = 0, cellMismatch = 0, corners = 0;
                float maxDifference = 0;
                const float correction = float(plane.WorldPlane.GetX()) * offset
                    + float(plane.WorldPlane.GetW()) - float(m_DistancePlane.GetW());
                for (const auto& source : patch.IslandSources)
                    for (uint32_t k = 0; k < 3; ++k, ++corners)
                    {
                        // 평면도 함께 움직이므로 w 변화가 복사본 X 이동 항을 상쇄한다.
                        const uint32_t skin = source[0];
                        const uint32_t v = model.m_LatticeTopology.Indices.at(skin).at(source[1] * 3 + k);
                        const float d = distances[corners] + correction;
                        const int8_t sign = std::abs(d) <= 1e-5f ? 0 : d > 0 ? 1 : -1;
                        signMismatch += sign != signs[skin][v];
                        cellMismatch += cells[corners] != model.m_LatticeTopology.VertexCell.at(skin).at(v);
                        maxDifference = std::max(maxDifference, std::abs(d - cpuDistances[skin][v]));
                    }

                distanceMatch = signMismatch == 0 && cellMismatch == 0;

                // quiet 옵션과 무관하게 최종 검증 판정을 출력한다.
                Utility::Printf("Island GPU verify %s: corners=%u signMismatch=%u maxDistanceDiff=%.9g cellMismatch=%u\n",
                    signMismatch == 0 && cellMismatch == 0 ? "PASS" : "FAIL", corners, signMismatch, maxDifference, cellMismatch);
            }
            catch (const std::exception& error)
            {
                // 예외 내용도 quiet 억제 없이 남긴다.
                Utility::Printf("Island GPU verify FAIL: %s\n", error.what());
            }
        }

        // 요청당 한 번 공통 분류기로 팔레트 joint의 몸체 그룹을 읽는다.
        cutPerfSigns.End();
        const std::vector<uint32_t> jointNodes(model.m_JointIndices.get(), model.m_JointIndices.get() + model.m_NumJoints);
        const auto groups = BuildCutBodyGroups(m_Parents, jointNodes, m_CutRoots);
        std::vector<uint32_t> jointGroups(model.m_NumJoints, 0);
        if (!groups.NodeGroups.empty())
        {
            for (uint32_t joint = 0; joint < model.m_NumJoints; ++joint)
                jointGroups[joint] = groups.NodeGroups[jointNodes[joint]];
        }

        // 실제 incoming 관절의 B 루트에서 자식으로 나가는 제출 포즈 교차를 찾는다.
        patch.Crossings.assign(m_CutRoots.size(), {});
        const auto first = std::find_if(m_SkinSections.begin(), m_SkinSections.end(),
            [](uint32_t skin)
            {
                // 공통 skin frame을 제공하는 첫 section을 고른다.
                return skin != UINT32_MAX;
            });
        for (size_t i = 0; i < m_CutRoots.size(); ++i)
        {
            // 각 P_i의 선택 자식과 교차점을 독립적으로 보존한다.
            auto& snapshot = patch.Crossings[i];
            snapshot.Root = m_CutRoots[i];
            snapshot.Child = m_CutChildren[i];
            if (snapshot.Root >= m_IslandNodes.size() || first == m_SkinSections.end())
            {
                continue;
            }
            const auto frame = m_SourceWorld * m_IslandFrames[size_t(first - m_SkinSections.begin())];
            const Math::Vector3 parent(frame * m_IslandNodes[snapshot.Root].GetW());
            const float a = Dot(plane.WorldPlane, Math::Vector4(parent + Math::Vector3(offset, 0, 0), 1));
            const Math::Vector3 center = (plane.WorldCorners[0] + plane.WorldCorners[2]) * 0.5f;
            float closest = std::numeric_limits<float>::max();
            for (uint32_t child = 0; child < m_Parents.size(); ++child)
            {
                // 보존한 선택 선분을 사용하고 snapshot이 없을 때만 직접 자식으로 제한한다.
                if (!IsSelectedSegmentChild(snapshot.Root, child, m_Parents[child], m_CutChildren[i]))
                    continue;

                // 제출 포즈의 선택 선분과 평면 사이의 교점을 계산한다.
                const Math::Vector3 position(frame * m_IslandNodes[child].GetW());
                const float b = Dot(plane.WorldPlane, Math::Vector4(position + Math::Vector3(offset, 0, 0), 1));
                float fraction = 0;
                uint32_t side = 0;
                if (!IntersectLatticeSegment(a, b, fraction, side))
                    continue;
                const Math::Vector3 crossing = parent + (position - parent) * fraction + Math::Vector3(offset, 0, 0);
                const float distance = LengthSquare(crossing - center);
                if (distance >= closest)
                    continue;

                // 자식 snapshot이 없으면 평면 중심에 가장 가까운 직접 자식 교차를 보존한다.
                closest = distance;
                snapshot.Point = crossing;
                snapshot.Child = child;
                snapshot.ChildSide = side;
                snapshot.Valid = true;
            }
        }

        // 투표 중에는 계층을 다시 탐색하지 않고 그룹 표만 조회한다.
        const auto group = [&](uint32_t joint)
        {
            // 팔레트 인덱스에 대응하는 사전 계산 그룹을 반환한다.
            return jointGroups.at(joint);
        };

        // 아일랜드 라벨링 호출의 CPU 시간을 측정한다.
        CutPerf::Cpu cutPerfLabel("LabelPatchIslands");
        patch.Islands = m_IslandLabels = LabelPatchIslands(model.m_LatticeTopology, model.m_SkinCpuSections,
            patch.IslandSources, signs, group);
        cutPerfLabel.End();

        // 검증된 셀만 역참조하고 GPU root와 CPU 섬 번호의 양방향 대응을 비교한다.
        if (m_IslandVerify && distanceMatch && m_IslandConverged)
        {
            try
            {
                // 성공한 Map은 비교 중 예외가 나도 범위 종료 시 해제한다.
                const auto unmap = [](ReadbackBuffer* buffer)
                {
                    // 회수 버퍼의 읽기 매핑을 해제한다.
                    buffer->Unmap();
                };
                const auto* parents = static_cast<const uint32_t*>(m_ParentReadback.Map());
                const std::unique_ptr<ReadbackBuffer, decltype(unmap)> parentMap(&m_ParentReadback, unmap);
                const auto* distances = static_cast<const float*>(m_DistanceReadback.Map());
                const std::unique_ptr<ReadbackBuffer, decltype(unmap)> distanceMap(&m_DistanceReadback, unmap);
                const auto* cells = static_cast<const uint32_t*>(m_CellReadback.Map());
                const std::unique_ptr<ReadbackBuffer, decltype(unmap)> cellMap(&m_CellReadback, unmap);

                // 없는 side까지 비교하고 활성 코너는 모두 같은 CPU 섬에 대응시킨다.
                std::map<uint32_t, uint32_t> gpuToCpu, cpuToGpu;
                uint32_t mismatch = 0;
                for (size_t t = 0; t < patch.IslandSources.size(); ++t)
                {
                    bool positive = false, negative = false;
                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        positive |= distances[t * 3 + k] > 1e-5f;
                        negative |= distances[t * 3 + k] < -1e-5f;
                    }
                    for (uint32_t side = 0; side < 2; ++side)
                    {
                        const uint32_t cpu = patch.Islands.TriangleSideIsland[t][side];
                        const bool exists = side == 0 ? positive || !negative : negative;
                        mismatch += exists != (cpu != UINT32_MAX);
                        if (!exists || cpu == UINT32_MAX)
                            continue;
                        for (uint32_t k = 0; k < 3; ++k)
                        {
                            const float d = distances[t * 3 + k];
                            if (side == 0 ? d < -1e-5f : d > 1e-5f)
                                continue;
                            const uint32_t root = parents[cells[t * 3 + k] * 2 + side];
                            mismatch += gpuToCpu.emplace(root, cpu).first->second != cpu;
                            mismatch += cpuToGpu.emplace(cpu, root).first->second != root;
                        }
                    }
                }
                partitionMatch = mismatch == 0;
                Utility::Printf("Island GPU partition %s: mismatch=%u\n", mismatch ? "FAIL" : "PASS", mismatch);
            }
            catch (const std::exception& error)
            {
                // 회수 실패도 quiet 옵션과 무관하게 검증 실패로 남긴다.
                Utility::Printf("Island GPU partition FAIL: %s\n", error.what());
            }
        }

        // 수렴한 GPU 번호 표를 보정 전 CPU 첫 등장 번호와 정확히 비교한다.
        if (m_IslandVerify && m_IslandConverged)
        {
            try
            {
                // 비교 중 예외에도 성공한 읽기 매핑을 해제한다.
                const auto unmap = [](ReadbackBuffer* buffer)
                {
                    // 번호 표 회수 버퍼의 매핑을 해제한다.
                    buffer->Unmap();
                };
                const auto* numbers = static_cast<const uint32_t*>(m_NumberReadback.Map());
                const std::unique_ptr<ReadbackBuffer, decltype(unmap)> numberMap(&m_NumberReadback, unmap);

                // 없는 side의 무효값까지 삼각형 양쪽을 각각 비교한다.
                uint32_t mismatch = 0;
                for (size_t t = 0; t < patch.IslandSources.size(); ++t)
                {
                    for (uint32_t side = 0; side < 2; ++side)
                    {
                        mismatch += numbers[t * 2 + side] != patch.Islands.TriangleSideIsland[t][side];
                    }
                }
                numberMatch = mismatch == 0 && m_GpuIslandCount == patch.Islands.IslandCount;
                Utility::Printf("Island GPU number %s: mismatch=%u islands=%u/%u\n",
                    mismatch || m_GpuIslandCount != patch.Islands.IslandCount ? "FAIL" : "PASS",
                    mismatch, m_GpuIslandCount, patch.Islands.IslandCount);
            }
            catch (const std::exception& error)
            {
                // 회수 실패도 quiet 옵션과 무관하게 검증 실패로 남긴다.
                Utility::Printf("Island GPU number FAIL: map=%s\n", error.what());
            }
        }

        // 근접·overflow·GPU 실패는 요청 전체의 double oracle 결과를 보존한다.
        if (m_IslandGpu && m_IslandConverged && !m_IslandDispatchLimited
            && distanceMatch && partitionMatch && numberMatch && m_VoteSupported)
        {
            try
            {
                // 비교 중 예외에도 성공한 회수 매핑을 해제한다.
                const auto unmap = [](ReadbackBuffer* buffer)
                {
                    // mirror의 읽기 매핑을 해제한다.
                    buffer->Unmap();
                };
                const auto* data = static_cast<const uint32_t*>(m_VoteReadback.Map());
                const std::unique_ptr<ReadbackBuffer, decltype(unmap)> voteMap(&m_VoteReadback, unmap);
                const auto* numbers = static_cast<const std::array<uint32_t, 2>*>(m_NumberReadback.Map());
                const std::unique_ptr<ReadbackBuffer, decltype(unmap)> numberMap(&m_NumberReadback, unmap);
                const uint32_t stride = m_VoteGroups + 5;
                const uint32_t table = m_VoteCapacity * 2 * stride;
                uint32_t nearCount = 0;
                for (uint32_t i = 0; i < m_GpuIslandCount; ++i)
                    nearCount += data[i * stride + m_VoteGroups + 4] != 0;
                const bool overflow = data[table + m_VoteCapacity * 2 + 2] != 0;
                const bool fallback = nearCount != 0 || overflow;
                bool metadataMatch = true;
                IslandLabels mirror = patch.Islands;
                if (!fallback)
                {
                    // GPU 번호·섬 메타데이터·몸체·기본값을 mirror로 교체한다.
                    mirror.IslandCount = m_GpuIslandCount;
                    // 동일 번호의 Scores·JointScores는 CPU oracle 값을 보존한다.
                    mirror.TriangleSideIsland.assign(numbers, numbers + patch.IslandSources.size());
                    std::memcpy(mirror.TriangleSideBody.data(), data + table, patch.IslandSources.size() * 8);
                    for (uint32_t i = 0; i < m_GpuIslandCount; ++i)
                    {
                        metadataMatch &= mirror.Islands[i].Triangles == data[i * stride + m_VoteGroups]
                            && mirror.Islands[i].Side == data[i * stride + m_VoteGroups + 1]
                            && mirror.Islands[i].Body == data[i * stride + m_VoteGroups + 2];
                        mirror.Islands[i].Triangles = data[i * stride + m_VoteGroups];
                        mirror.Islands[i].Side = data[i * stride + m_VoteGroups + 1];
                        mirror.Islands[i].Body = data[i * stride + m_VoteGroups + 2];
                    }
                    std::memcpy(mirror.SideDefault, data + table + m_VoteCapacity * 2, 8);
                }

                // 근접 해결 뒤 모든 삼각형 side와 두 기본값을 정확히 비교한다.
                uint32_t mismatch = mirror.SideDefault[0] != patch.Islands.SideDefault[0];
                mismatch += mirror.SideDefault[1] != patch.Islands.SideDefault[1];
                for (size_t t = 0; t < patch.IslandSources.size(); ++t)
                    for (uint32_t side = 0; side < 2; ++side)
                        mismatch += mirror.TriangleSideBody[t][side] != patch.Islands.TriangleSideBody[t][side];
                if (m_IslandVerify)
                    Utility::Printf("Island GPU vote %s: mismatch=%u near=%u islands=%u\n", mismatch ? "FAIL" : "PASS", mismatch, nearCount, m_GpuIslandCount);
                const char* reason = overflow ? "overflow" : nearCount ? "near"
                    : !metadataMatch ? "metadata" : mismatch ? "vote" : nullptr;
                if (reason)
                    Utility::Printf("Island GPU UF fallback reason=%s\n", reason);
                else if (m_ConsumeGpu)
                {
                    // 후속 cap 재라벨과 분류에 동일 번호의 점수와 소비 표를 전달한다.
                    patch.Islands = m_IslandLabels = std::move(mirror);
                    m_ClassifyGpuReady = true;
                    Utility::Printf("Island GPU vote consumed: near=%u islands=%u\n", nearCount, m_GpuIslandCount);
                }
            }
            catch (const std::exception& error)
            {
                // 회수 실패는 CPU oracle을 보존하고 quiet와 무관하게 출력한다.
                Utility::Printf("Island GPU UF fallback reason=vote-map %s\n", error.what());
                if (m_IslandVerify)
                    Utility::Printf("Island GPU vote FAIL: mismatch=1 near=0 islands=%u\n", m_GpuIslandCount);
            }
        }
        else if (m_IslandGpu)
        {
            // 첫 실패 검사를 quiet와 무관하게 남기고 oracle 출력을 보존한다.
            const char* reason = !m_IslandConverged ? "not-converged" : !distanceMatch ? "distance-sign/cell"
                : !partitionMatch ? "partition" : !numberMatch ? "number/count" : "dynamic-root";
            Utility::Printf("Island GPU UF fallback reason=%s\n", reason);
        }

        // 선택한 최종 섬 표의 기존 진단을 이어간다.
        uint32_t reassigned = 0;
        for (const auto& island : patch.Islands.Islands)
            reassigned += island.Body != uint32_t(island.Side == 0 ? !positiveA : positiveA);
        PatchLog("MI2 islands=%u reassigned=%u side0Default=%u side1Default=%u\n", patch.Islands.IslandCount,
            reassigned, patch.Islands.SideDefault[0], patch.Islands.SideDefault[1]);

        // 최대 기여 joint 이름과 점수로 떨어진 아일랜드의 소속 근거를 보인다.
        for (const auto& island : patch.Islands.Islands)
        {
            uint32_t joint = UINT32_MAX;
            double score = 0;
            for (const auto& entry : island.JointScores)
                if (entry.second > score)
                {
                    joint = entry.first;
                    score = entry.second;
                }
            const uint32_t node = joint == UINT32_MAX ? UINT32_MAX : model.m_JointIndices[joint];
            PatchLog("MI2 island side=%u tris=%u scoreA=%.6f scoreB=%.6f body=%u representative=%u name=%s weight=%.6f\n",
                island.Side, island.Triangles, island.Scores[0], island.Scores.size() > 1 ? island.Scores[1] : 0,
                island.Body, node, SafeDiagnosticNodeName(modelName, node).c_str(), score);
        }
    }

    // 제출 프레임의 제거 상태와 GPU 입력으로 CPU 개수와 압축 출처 순서를 검사한다.
    void SkeletalPatchJob::Validate(const SkeletalPatchResult& result)
    {
        // GPU가 완료된 진단 snapshot만 읽고 평상시 CPU 추출은 실행하지 않는다.
        using namespace PatchReference;
        Input input;
        input.Plane = m_Plane;
        input.State = m_State;
        const auto& model = *m_Input->GetSource();
        input.JointNodes.assign(model.m_JointIndices.get(), model.m_JointIndices.get() + model.m_NumJoints);
        const auto* data = static_cast<const uint8_t*>(m_SourceReadback.Map());
        const auto* pose = static_cast<const uint8_t*>(m_PoseReadback.Map());
        for (uint32_t s = 0; s < m_Input->GetSections().size(); ++s)
        {
            const auto& mesh = m_Input->GetMesh(s);
            Section section;
            section.StartJoint = mesh.startJoint;
            section.JointCount = mesh.numJoints;
            for (uint32_t i = 0; i < mesh.vbSize / mesh.vbStride; ++i)
            {
                Vertex vertex{};
                float position[3];
                std::memcpy(position, pose + m_PoseOffsets[s] + i * 40, 12);
                vertex.Position = Math::Vector3(position[0], position[1], position[2]);
                if (mesh.psoFlags & PSOFlags::kHasSkin)
                {
                    const auto* skin = data + mesh.vbOffset + (i + 1) * mesh.vbStride - 16;
                    std::memcpy(vertex.Palette.data(), skin, 8);
                    std::memcpy(vertex.Weights.data(), skin + 8, 8);
                }
                section.Vertices.push_back(vertex);
            }
            if (mesh.ibFormat == DXGI_FORMAT_R32_UINT)
            {
                section.Indices32.resize(mesh.ibSize / 4);
                std::memcpy(section.Indices32.data(), data + mesh.ibOffset, mesh.ibSize);
            }
            else
            {
                section.Indices16.resize(mesh.ibSize / 2);
                std::memcpy(section.Indices16.data(), data + mesh.ibOffset, mesh.ibSize);
            }
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
                section.Draws.push_back({ mesh.draw[d].startIndex, mesh.draw[d].primCount, int32_t(mesh.draw[d].baseVertex) });
            input.Sections.push_back(std::move(section));
        }
        m_SourceReadback.Unmap(); m_PoseReadback.Unmap();

        // CPU 기준의 원본 순서와 GPU compact source list를 항목별로 대조한다.
        if (m_Influence)
        {
            const uint32_t reference = SelectInfluenceJointReference(input);
            PatchLog("Skeletal patch Influence GPU==CPU NodeId GPU=%u CPU=%u %s\n", m_Node, reference,
                m_Node == reference ? "PASS" : "FAIL");
            if (m_Node != reference) throw std::runtime_error("Influence selected NodeId mismatch");
        }
        const auto expected = ExtractPatchReference(input, m_Node, m_Influence
            ? std::vector<std::vector<uint32_t>>{} : m_SelectionMasks);

        // 추출·유지 삼각형이 공유하는 원본 정점만 외곽 경계로 기록한다.
        // 재질·UV 이음매의 중복 인덱스도 같은 위치의 원본 경계로 묶는다.
        const auto key = [](const Math::Vector3& p)
        {
            // 10 마이크로 로컬 단위 격자로 GPU의 같은 위치를 대응시킨다.
            return std::make_tuple(std::llround(float(p.GetX()) * 1e5), std::llround(float(p.GetY()) * 1e5),
                std::llround(float(p.GetZ()) * 1e5));
        };
        std::set<std::tuple<int64_t, int64_t, int64_t>> selectedVertices;
        for (const auto& triangle : expected)
        {
            for (uint32_t index : triangle.Vertices) selectedVertices.insert(key(input.Sections[triangle.SectionId].Vertices[index].Position));
        }
        const std::set<uint32_t> extracted(m_Output->Triangles.begin(), m_Output->Triangles.end());
        const size_t duplicates = m_Output->Triangles.size() - extracted.size();
        if (duplicates != 0)
            throw std::runtime_error("Duplicate patch triangle");
        uint32_t globalBoundary = 0;
        std::set<std::pair<uint32_t, uint32_t>> recorded;
        for (uint32_t section = 0; section < input.Sections.size(); ++section)
            for (const auto& draw : input.Sections[section].Draws)
                for (uint32_t t = 0; t < draw.IndexCount / 3; ++t, ++globalBoundary)
                {
                    if ((!input.State.empty() && (input.State[globalBoundary] & 0x80000000u))
                        || extracted.count(globalBoundary)) continue;
                    for (uint32_t index : TriangleIndices(input.Sections[section], draw, t))
                        if (selectedVertices.count(key(input.Sections[section].Vertices[index].Position)) && recorded.emplace(section, index).second)
                            m_Output->Boundary.push_back({ section, index, input.Sections[section].Vertices[index].Position });
                }
        PatchLog("SP7e2 extracted boundary=%zu selectedPositions=%zu\n", m_Output->Boundary.size(), selectedVertices.size());

        // PS-1 체크에서만 같은 경계 집합의 전역 joint와 원본 정규화 전 weight를 보존한다.
        uint32_t skinChecks = 0;
        CommandLineArgs::GetInteger(L"skeletal-patch-break-checks", skinChecks);
        if (skinChecks)
            for (const auto& boundary : m_Output->Boundary)
            {
                const auto& section = input.Sections[boundary.Section];
                const auto& vertex = section.Vertices[boundary.Index];
                SkeletalPatchBuffers::BoundarySkin skin{};
                for (uint32_t slot = 0; slot < 4; ++slot)
                {
                    skin.Joints[slot] = section.StartJoint + vertex.Palette[slot];
                    skin.Weights[slot] = vertex.Weights[slot];
                }
                m_Output->BoundarySkins.push_back(skin);
            }

        // 원본 pose와 실제 packed VB를 게시 TRS로 변환해 오프셋 제거 후 월드 경계를 대조한다.
        const auto* packed = static_cast<const uint8_t*>(m_OutputReadback.Map());
        Math::Vector3 sourceMin(FLT_MAX), sourceMax(-FLT_MAX), copyMin(FLT_MAX), copyMax(-FLT_MAX);
        SceneTransform copyWorld;
        if (!SceneTransform::TryFromAffine(Math::AffineTransform(m_SourceWorld), copyWorld))
            throw std::runtime_error("Skeletal patch diagnostic world is not TRS");
        const Math::Vector3 offset(m_Output->SourceWorldWidth + 0.5f, 0, 0);
        copyWorld.SetTranslation(copyWorld.GetTranslation() + offset);
        float maxError = 0;
        for (uint32_t t = 0; t < expected.size() && t < result.PatchTriangles; ++t)
        {
            const auto& origin = expected[t];
            const auto& section = input.Sections[origin.SectionId];
            const auto indices = TriangleIndices(section, section.Draws[origin.DrawId], origin.TriangleId);
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                float position[3];
                std::memcpy(position, packed + (t * 3 + corner) * 28, 12);
                const Math::Vector3 source = Math::Vector3(m_SourceWorld
                    * Math::Vector4(section.Vertices[indices[corner]].Position, 1));
                const Math::Vector3 copy = copyWorld.ToAffine()
                    * Math::Vector3(position[0], position[1], position[2]) - offset;
                sourceMin = Math::Min(sourceMin, source); sourceMax = Math::Max(sourceMax, source);
                copyMin = Math::Min(copyMin, copy); copyMax = Math::Max(copyMax, copy);
                maxError = std::max(maxError, float(Math::Length(source - copy)));
            }
        }
        m_OutputReadback.Unmap();

        // 진단 입력의 전체 draw에서 구한 월드 X 폭과 GPU의 두 스칼라 집계를 비교한다.
        float sourceXMin = FLT_MAX, sourceXMax = -FLT_MAX;
        for (const auto& section : input.Sections)
            for (const auto& draw : section.Draws)
                for (uint32_t t = 0; t < draw.IndexCount / 3; ++t)
                    for (uint32_t i : TriangleIndices(section, draw, t))
                    {
                        const float x = float((m_SourceWorld * Math::Vector4(section.Vertices[i].Position, 1)).GetX());
                        sourceXMin = std::min(sourceXMin, x); sourceXMax = std::max(sourceXMax, x);
                    }
        const float widthError = std::abs(sourceXMax - sourceXMin - m_Output->SourceWorldWidth);
        PatchLog("Skeletal patch source world X CPU=[%.6f,%.6f] widthError=%.9f %s\n",
            sourceXMin, sourceXMax, widthError, widthError < 1e-4f ? "PASS" : "FAIL");

        // 빈 패치에는 실제 경계가 없으므로 0으로 표시하고 영향 부위의 오차를 남긴다.
        if (expected.empty()) sourceMin = sourceMax = copyMin = copyMax = Math::Vector3(Math::kZero);
        PatchLog("Skeletal patch source region world min=(%.6f,%.6f,%.6f) max=(%.6f,%.6f,%.6f)\n",
            float(sourceMin.GetX()), float(sourceMin.GetY()), float(sourceMin.GetZ()),
            float(sourceMax.GetX()), float(sourceMax.GetY()), float(sourceMax.GetZ()));
        PatchLog("Skeletal patch copy minus offset min=(%.6f,%.6f,%.6f) max=(%.6f,%.6f,%.6f) maxVertexError=%.9f %s\n",
            float(copyMin.GetX()), float(copyMin.GetY()), float(copyMin.GetZ()),
            float(copyMax.GetX()), float(copyMax.GetY()), float(copyMax.GetZ()), maxError, maxError < 1e-4f ? "PASS" : "FAIL");

        // 선택 패치의 무한 평면 교차와 사각형 밖 끝점을 진단에서만 측정한다.
        uint32_t patchInfinite = 0, patchFinite = 0, outsideTriangles = 0;
        Math::Vector3 crossMin(FLT_MAX), crossMax(-FLT_MAX);
        for (const auto& origin : expected)
        {
            const auto& section = input.Sections[origin.SectionId];
            const auto indices = TriangleIndices(section, section.Draws[origin.DrawId], origin.TriangleId);
            Math::Vector3 points[3];
            float distances[3];
            for (uint32_t i = 0; i < 3; ++i)
            {
                points[i] = section.Vertices[indices[i]].Position;
                distances[i] = float(Math::Dot(Math::Vector4(points[i], 1), input.Plane.WorldPlane));
            }
            if (*std::min_element(distances, distances + 3) >= -input.Epsilon
                || *std::max_element(distances, distances + 3) <= input.Epsilon) continue;
            ++patchInfinite;
            patchFinite += CrossesRectangle(input, section, indices) ? 1u : 0u;
            bool outside = false;
            for (uint32_t i = 0; i < 3; ++i)
            {
                const uint32_t j = (i + 1) % 3;
                if (distances[i] * distances[j] >= 0) continue;
                const auto hit = points[i] + (points[j] - points[i]) * (distances[i] / (distances[i] - distances[j]));
                const Math::Vector3 rectangle(input.Plane.WorldToPlane * Math::Vector4(hit, 1));
                crossMin = Math::Min(crossMin, rectangle); crossMax = Math::Max(crossMax, rectangle);
                const float dx = std::max(0.0f, std::abs(float(rectangle.GetX())) - input.Plane.HalfWidth);
                const float dz = std::max(0.0f, std::abs(float(rectangle.GetZ())) - input.Plane.HalfHeight);
                if (std::max(dx, dz) <= input.Epsilon) continue;
                outside = true;
                const Math::Vector3 worldHit(m_SourceWorld * Math::Vector4(hit, 1));
                PatchLog("Skeletal patch outside section=%u draw=%u triangle=%u world=(%.6f,%.6f,%.6f) rectangleXZ=(%.6f,%.6f) excessXZ=(%.6f,%.6f)\n",
                    origin.SectionId, origin.DrawId, origin.TriangleId, float(worldHit.GetX()), float(worldHit.GetY()), float(worldHit.GetZ()),
                    float(rectangle.GetX()), float(rectangle.GetZ()), dx, dz);
            }
            if (outside)
            {
                ++outsideTriangles;
                for (uint32_t index : indices)
                {
                    const auto& vertex = section.Vertices[index];
                    PatchLog("Skeletal patch outside weights triangle=%u nodes=(%u,%u,%u,%u) weights=(%u,%u,%u,%u)\n", origin.TriangleId,
                        input.JointNodes[section.StartJoint + vertex.Palette[0]], input.JointNodes[section.StartJoint + vertex.Palette[1]],
                        input.JointNodes[section.StartJoint + vertex.Palette[2]], input.JointNodes[section.StartJoint + vertex.Palette[3]],
                        vertex.Weights[0], vertex.Weights[1], vertex.Weights[2], vertex.Weights[3]);
                }
            }
        }
        // 교차가 없으면 경계 sentinel 대신 빈 범위를 기록한다.
        if (!patchInfinite) crossMin = crossMax = Math::Vector3(Math::kZero);
        PatchLog("Skeletal patch cross-section infinite=%u finite=%u outsideTriangles=%u rectangle=(%.6f,%.6f) rangeXZ=[%.6f,%.6f]x[%.6f,%.6f]\n",
            patchInfinite, patchFinite, outsideTriangles, input.Plane.HalfWidth * 2, input.Plane.HalfHeight * 2,
            float(crossMin.GetX()), float(crossMax.GetX()), float(crossMin.GetZ()), float(crossMax.GetZ()));

        // CPU 기준의 개수와 출처 및 게시 위치를 모두 성공 조건에 포함한다.
        const auto* origins = static_cast<const uint32_t*>(m_OriginsReadback.Map());
        bool pass = expected.size() == result.PatchTriangles && maxError < 1e-4f && widthError < 1e-4f;
        for (uint32_t i = 0; i < expected.size() && i < result.PatchTriangles; ++i)
            pass &= origins[i * 3] == expected[i].SectionId && origins[i * 3 + 1] == expected[i].DrawId
                && origins[i * 3 + 2] == expected[i].TriangleId;

        // 내부 Mesh별 출처 개수도 기록하여 모든 팔레트가 비교에 포함됐는지 확인한다.
        for (uint32_t section = 0; section < input.Sections.size(); ++section)
        {
            uint32_t cpu = 0, gpu = 0;
            for (const auto& source : expected) cpu += source.SectionId == section;
            for (uint32_t i = 0; i < result.PatchTriangles; ++i) gpu += origins[i * 3] == section;
            PatchLog("Skeletal patch sources section=%u startJoint=%u CPU=%u GPU=%u %s\n",
                section, input.Sections[section].StartJoint, cpu, gpu, cpu == gpu ? "PASS" : "FAIL");
        }
        m_OriginsReadback.Unmap();

        // 선택 수와 출처 및 위치 비교 결과를 최종 진단으로 기록한다.
        PatchLog("Skeletal patch CPU patch=%zu sources=%s %s\n", expected.size(),
            pass ? "match" : "mismatch", pass ? "PASS" : "FAIL");
        if (!pass) throw std::runtime_error("Skeletal patch GPU/CPU diagnostic FAIL");
        PatchLog("MI4b GPU==CPU PASS triangles=%zu duplicates=%zu\n", expected.size(), duplicates);
    }
}
