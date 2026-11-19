#include "SkeletalPatchCopy.h"
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
#include "CompiledShaders/PatchBodyClassifyCS.h"
#include "CompiledShaders/PatchBodyCompactCS.h"
#include "tests/SkeletalPatchReference.h"
#include <cstdio>
#include <cstdarg>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <cstring>
#include <fstream>
#include <set>
#include <map>
#include <tuple>
#include <sstream>
#include "SystemTime.h"
#include "../../Model/json.hpp"

namespace Viewer
{
    namespace
    {
        RootSignature PatchRoot;
        ComputePSO PatchPSO(L"Extract skeletal patch");
        struct DrawInput
        {
            uint32_t VbOffset, Stride, IbOffset, Index32;
            uint32_t StartIndex, BaseVertex, Triangles, PoseOffset;
            uint32_t FirstTriangle, MaskOffset, Flags, SectionId;
            uint32_t DrawId, JointCount, VertexCount, Reserved1;
        };
        struct Parameters
        {
            Math::Vector4 Plane;
            Math::Matrix4 LocalToRectangle;
            Math::Matrix4 LocalToWorld;
            float Epsilon, HalfWidth, HalfHeight;
            uint32_t Triangles, Draws, DepthOffset, IndexOffset, Mode;
            uint32_t NodeCount, Influence, HasState, Padding1;
        };
        static_assert(sizeof(DrawInput) == 64 && sizeof(Parameters) == 192, "Patch shader layout");
        static_assert(sizeof(SkeletalPatchResult) == 12, "Patch counts readback layout");

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
            struct Constants
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

        // mini에 없는 이름은 진단에서만 원본 GLB의 DFS 순서로 복원한다.
        std::string DiagnosticNodeName(const std::string& model, uint32_t node)
        {
            // JSON chunk를 읽고 변환기와 같은 scene root 전위 순서를 따른다.
            std::ifstream file("Models/" + model + ".glb", std::ios::binary);
            uint32_t header[5]{};
            file.read(reinterpret_cast<char*>(header), sizeof(header));
            if (!file || header[0] != 0x46546c67 || header[4] != 0x4e4f534a) return "unavailable";
            std::string bytes(header[3], ' ');
            file.read(&bytes[0], bytes.size());
            const auto json = nlohmann::json::parse(bytes);
            const auto roots = json["scenes"][json.value("scene", 0)]["nodes"].get<std::vector<uint32_t>>();
            std::vector<uint32_t> stack(roots.rbegin(), roots.rend());
            for (uint32_t id = 0; !stack.empty(); ++id)
            {
                const auto current = stack.back();
                stack.pop_back();
                if (id == node) return json["nodes"][current].value("name", std::string("unnamed"));
                const auto children = json["nodes"][current].value("children", std::vector<uint32_t>{});
                stack.insert(stack.end(), children.rbegin(), children.rend());
            }
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

        // 투표·선택·추출 패스가 공유하는 root signature와 compute PSO를 준비한다.
        void InitializePatch()
        {
            // 여러 C 요청 사이에 파이프라인을 재사용한다.
            static bool initialized = false;
            if (initialized) return;
            PatchRoot.Reset(12);
            PatchRoot[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 4; ++i) PatchRoot[i + 1].InitAsBufferSRV(i);
            for (uint32_t i = 0; i < 6; ++i) PatchRoot[i + 5].InitAsBufferUAV(i);
            PatchRoot[11].InitAsBufferSRV(4);
            PatchRoot.Finalize(L"Skeletal patch extraction");
            PatchPSO.SetRootSignature(PatchRoot);
            PatchPSO.SetComputeShader(g_pPatchExtractCS, sizeof(g_pPatchExtractCS));
            PatchPSO.Finalize();
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
    };

    // 유한 평면 hit에서 Kinematic 선분을 선택하고 재요청 거절 근거를 수집한다.
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
            if (!skeletal.ReadDiagnosticState(owner, state) || body >= state.Bodies.size()) continue;
            for (const auto& stateBody : state.Bodies)
                if (stateBody.Motion == MiniPhysics::BodyMotion::Dynamic)
                    result.DynamicNodes[owner].insert(stateBody.Node);
            const auto& hitBody = state.Bodies[body];
            if (hitBody.Motion == MiniPhysics::BodyMotion::Dynamic) continue;
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

            // 원래 hit에 끝점이 포함된 끊김 또는 Dynamic 선분만 거절 근거로 쓴다.
            for (const auto& segment : unavailable)
            {
                const auto& original = result.OriginalHits.at(entry.first);
                if (!original.count(segment.ParentNode) && !original.count(segment.ChildNode)) continue;
                Vector3 point;
                if (intersect(segment, point)) result.AlreadyBroken = true;
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

    // 두 node의 현재 월드 중점에서 본 방향을 법선으로 하는 절단 평면을 구성한다.
    MeshCutting::CutPlaneData BuildSkeletalBonePlane(const GameObject& object, uint32_t node, uint32_t child)
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

        // 두 본의 중점에서 유한 절단 평면을 생성하고 유효성을 검사한다.
        MeshCutting::CutPlaneData plane;
        if (!MeshCutting::TryBuildCutPlaneData({ 0.3f, 0.3f },
            Math::AffineTransform(Math::Matrix3(x, normal, Math::Cross(x, normal)), (a + b) * 0.5f), plane))
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
                const float distance = float(Math::Length(positions[pair.Rim] - positions[pair.Cap]));
                const double change = std::abs(double(distance) - double(pair.DistanceAtCut)) * 1000.0;
                maximum = std::isfinite(change) ? std::max(maximum, change) : INFINITY;
            }
        }

        // 스키닝·전체 정점 경계·대응 쌍·0.01 mm 간격을 모두 만족해야 통과한다.
        const bool pass = skinned && inBounds && pairCount > 0 && maximum <= 0.01;
        PatchLog("PS4 case=%u side=%s frame=%s skinned=%d inBounds=%d rimCapPairs=%u rimCapChangeMax_mm=%.6f result=%s\n",
            diagnosticCase, side, frame, int(skinned), int(inBounds), pairCount, maximum, pass ? "PASS" : "FAIL");
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
                        if (dynamic != rig.DynamicNodes.end() && (dynamic->second.count(legacy.NodeId) || dynamic->second.count(legacy.ChildNodeId)))
                            line << " reason=legacy_endpoint_Dynamic";
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
            m_SkinSnapshots.resize(boneCut ? 1 : groups.AttachNodes.size());
            m_SkinPaletteErrors.assign(m_SkinSnapshots.size(), 0);
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

                // 체크 실행에서만 실제 t_cut 입력으로 팔레트 오차를 계산한다.
                m_SkinPaletteErrors[side] = 0;
                if (m_SkinCheckCase)
                {
                    std::vector<Joint> palette;
                    BuildPatchPalette(snapshot, joints, nodes, palette);
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
        const Matrix4 world(mesh->GetWorldTransform(target->GetTransform()));
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
        m_BonePending = boneCut && submitted;
        if (!submitted && diagnostic)
            throw std::runtime_error("Skeletal patch diagnostic submit rejected");
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

    // 생성 skin VB를 보관하고 진단 옵션에서 Data 비교와 rim/cap 대응 쌍 생성을 수행한다.
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
        const bool built = BuildPatchSkinVertices(*m_SourceRevision, patch.TriangleSources,
            geometry, snapshot, skin->Vertices, patch.Islands.TriangleSideBody);
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

            // 현재 pose 대신 실제 t_cut joint와 부착 node를 전달하여 고정 본과 K를 복원한다.
            std::vector<Math::Matrix4> nodes(size_t(snapshot.AttachNode) + 1, Math::Matrix4(Math::kIdentity));
            nodes[snapshot.AttachNode] = snapshot.AttachAtCut;
            std::vector<Joint> palette;
            BuildPatchPalette(snapshot, snapshot.JointsAtCut.data(), nodes.data(), palette);
            const auto* bind = static_cast<const uint8_t*>(readbacks[0].Map());
            const auto* data = static_cast<const uint8_t*>(readbacks[1].Map());
            const auto* provenance = static_cast<const uint32_t*>(readbacks[2].Map());
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

                    // 내부 교점은 GPU와 같이 두 원본 끝점을 각각 skin한 뒤 t로 보간한다.
                    if ((skin->Vertices.Flags & kSkinExtFlag) && *reinterpret_cast<const uint32_t*>(input + skin->Vertices.Stride - kSkinExtInternal))
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
                    const auto* origin = provenance + size_t(v) * 8;
                    if (origin[0] == UINT32_MAX)
                    {
                        ++cap;
                        capVertices.push_back(v);
                    }
                    else if (origin[2] == origin[3])
                        ++copied;
                    else
                    {
                        ++split;
                        // 실제 경계만 cap과 짝지으며 내부 교점은 표면 수에만 포함한다.
                        if (!*reinterpret_cast<const uint32_t*>(input + skin->Vertices.Stride - kSkinExtInternal))
                            rimVertices.push_back(v);
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
                    skin->RimCapPairs.push_back({ rim, match, nearest });
                }

                // split·cap 수, 누락 수, t_cut 최근접 거리 최댓값을 원인 조사용으로 남긴다.
                PatchLog("PS4 seam pairs rim=%zu cap=%zu unmatched=%u nearestAtCutMax_mm=%.6f\n",
                    rimVertices.size(), capVertices.size(), unmatched, double(nearestMax) * 1000.0);
            }

            // 동기 진단에서 사용한 세 버퍼의 매핑을 해제한다.
            for (auto& readback : readbacks)
                readback.Unmap();
        }

        // 생성 실패와 빈 결과도 오차 한계로 PASS가 되지 않게 같은 형식으로 기록한다.
        if (!vertices)
            maxError = INFINITY;
        PatchLog("PS2 case=%u side=%s vertices=%u copied=%u split=%u cap=%u maxError_mm=%.6f result=%s\n",
            m_SkinCheckCase, label, vertices, copied, split, cap, maxError, maxError <= 0.01 ? "PASS" : "FAIL");

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
            SkeletalRigState state;
            m_Physics->GetSkeletalPhysics().ReadDiagnosticState(m_Break.Owner, state);
            unsigned dynamic = 0;
            for (const auto& body : state.Bodies)
                dynamic += body.Motion == MiniPhysics::BodyMotion::Dynamic;
            PatchLog("SP7b RequestId=%llu BreakStatus=%u JointId=%u ParentNode=%u Dynamic=%u\n",
                m_RequestId, unsigned(status), m_Break.JointId, m_Incoming.ParentNode,
                dynamic >= m_DynamicBefore ? dynamic - m_DynamicBefore : 0);
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

                        // 게시 전 준비 목록에 조각을 보관한다.
                        pieces.push_back(std::move(piece));
                    }
                    if (!m_DemoUnmask && !source->SetTriangleMask(m_Removal))
                        throw std::runtime_error("SP7e invalid removal mask");
                    for (auto& piece : pieces)
                        owner->CommitMeshComponent(std::move(piece));
                    PatchLog("SP7e published owner=%llu patchTriangles=%u hidden=%u nodes=%u/%u\n",
                        m_Break.Owner, m_Result.PatchTriangles, m_DemoUnmask ? 0u : 1u, m_Attachments[0].NodeId, m_Attachments[1].NodeId);

                    // 같은 프레임에 치환 IB와 새 조각의 렌더 상수 준비를 끝낸다.
                    auto& context = GraphicsContext::Begin(L"Publish attached skeletal pieces");
                    scene.PrepareMeshes(context);
                    context.Finish();

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
        CheckPatchSkin(*patch);
        m_SkinPatch = !m_SkinSnapshots.empty() ? patch : nullptr;
        m_SelectedNode = m_Job.SelectedNode();
        if (!result.PatchTriangles)
        {
            PatchLog("Skeletal patch publish skipped: %s; no copy or cut requested\n",
                result.CrossingTriangles ? "no influenced triangles" : "no finite rectangle crossing triangles");
            PatchLog("Skeletal patch cut result: rejected %s components=0 caps=0 uncapped=0 error=0 gap=0\n",
                "no influenced triangles");
            if (m_Diagnostic && scene.FindObject(m_CopyOwner))
                throw std::runtime_error("Skeletal patch zero crossing created a copy");
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
        const auto counts = RelabelPatchIslands(relabeled, requests);
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

    // cutter 출력의 실제 section 범위를 n+1 몸체로 판정하고 압축용 표를 보관한다.
    void SkeletalPatchCopy::ClassifyCutBodies(const GeometryBuffers& geometry, const IslandLabels& islands,
        const std::vector<std::array<uint32_t, 2>>& capLoops, uint32_t side, uint32_t bodyCount)
    {
        // 합성 압축 검사는 진단 옵션에서만 실행한다.
        CheckBodyCompaction();
        m_DroppedCaps[side] = 0;

        // 몸체 수는 라벨 최댓값이 아닌 요청 그룹 수이며 빈 마지막 몸체도 포함한다.
        if (!bodyCount || bodyCount > (UINT32_MAX / 4 - 10) / 2)
            throw std::length_error("MI4d invalid classification body count");

        // 표는 [패치 삼각형][반공간] 순서이며 물리 A/B 순서와 무관하다.
        if (islands.TriangleSideBody.empty()) return;
        if (side == 0)
        {
            m_CapLoopTable.Create(L"Cap loop side bodies", uint32_t(capLoops.size()), sizeof(capLoops[0]), capLoops.data());
            m_IslandTable.Create(L"Island triangle sides", uint32_t(islands.TriangleSideBody.size()),
                sizeof(islands.TriangleSideBody[0]), islands.TriangleSideBody.data());
        }
        struct Constants
        {
            uint32_t FirstTriangle, TriangleCount, PatchTriangles, Side;
            uint32_t DefaultBody, CapLoopCount, BodyCount, Padding;
        };
        static_assert(sizeof(Constants) == 32, "Body classification constants");

        // 읽기 전용 provenance·표와 판정 결과·카운터만 compute에 바인딩한다.
        static RootSignature root;
        static ComputePSO pso(L"Classify patch output bodies");
        static bool initialized = false;
        if (!initialized)
        {
            root.Reset(6);
            root[0].InitAsConstantBuffer(0);
            root[1].InitAsBufferSRV(0);
            root[2].InitAsBufferSRV(1);
            root[3].InitAsBufferUAV(0);
            root[4].InitAsBufferUAV(1);
            root[5].InitAsBufferSRV(2);
            root.Finalize(L"Patch body classification");
            pso.SetRootSignature(root);
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
        if (!end) return;
        auto& bodies = m_TriangleBodies[side];
        bodies.Create(L"Output triangle bodies", end, sizeof(uint32_t));
        ByteAddressBuffer counters;
        ReadbackBuffer readback;
        const uint32_t counterCount = 10 + bodyCount * 2;
        counters.Create(L"Body classification counters", counterCount, sizeof(uint32_t));
        readback.Create(L"Body classification readback", counterCount, sizeof(uint32_t));

        // 각 section의 실제 출력 삼각형만 dispatch하고 합산 카운터를 초기화한다.
        auto& graphics = GraphicsContext::Begin(L"Classify patch output bodies");
        auto& compute = graphics.GetComputeContext();
        auto& provenance = const_cast<ByteAddressBuffer&>(geometry.Provenance);
        compute.TransitionResource(provenance, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(m_IslandTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(m_CapLoopTable, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(bodies, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.TransitionResource(counters, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.ClearUAV(counters);
        compute.InsertUAVBarrier(counters);
        compute.SetRootSignature(root);
        compute.SetPipelineState(pso);
        compute.SetBufferSRV(1, provenance);
        compute.SetBufferSRV(2, m_IslandTable);
        compute.SetBufferSRV(5, m_CapLoopTable);
        compute.SetBufferUAV(3, bodies);
        compute.SetBufferUAV(4, counters);
        for (const auto& mesh : geometry.Meshes)
        {
            Constants c{ mesh.vbOffset / 84, mesh.vbSize / 84, uint32_t(islands.TriangleSideBody.size()),
                side, islands.SideDefault[side], uint32_t(capLoops.size()), bodyCount, 0 };
            if (!c.TriangleCount) continue;
            compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
            const uint32_t groups = (c.TriangleCount + 63) / 64;
            compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
        }

        // 임시 자원은 완료까지 유지하고 provenance와 판정 결과를 후속 읽기 상태로 돌린다.
        graphics.CopyBuffer(readback, counters);
        graphics.TransitionResource(provenance, D3D12_RESOURCE_STATE_GENERIC_READ);
        graphics.TransitionResource(bodies, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        graphics.Finish(true);
        const auto* counts = static_cast<const uint32_t*>(readback.Map());
        PatchLog("MI3a side=%u tris=%u cap=%u body0=%u body1=%u invalid=%u idMismatch=%u reassigned=%u\n",
            side, counts[0], counts[1], counts[2], counts[3], counts[4], counts[5], counts[6]);
        m_DroppedCaps[side] = counts[7];
        PatchLog("MI3f side=%u cap=%u dropped=%u kept=%u fallback=%u invalid=%u\n",
            side, counts[1], counts[7], counts[1] - counts[7], counts[8], counts[9]);

        // 같은 side의 여러 몸체를 별도 카운터로 남겨 후속 loop 귀속을 검증한다.
        for (uint32_t body = 0; body < bodyCount; ++body)
        {
            PatchLog("MI4d classify side=%u body=%u surface=%u cap=%u\n",
                side, body, counts[10 + body], counts[10 + bodyCount + body]);
        }
        readback.Unmap();
    }

    struct SkeletalPatchCopy::BodyCompactJob
    {
        std::shared_ptr<const GeometryBuffers> Inputs[2];
        Microsoft::WRL::ComPtr<ID3D12Resource> Bodies[2];
        std::shared_ptr<GeometryBuffers> Output;
        ByteAddressBuffer Layouts, Bins, Status;
        ReadbackBuffer Metadata, Flags;
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

    // 완료된 n+1 몸체 압축을 회수하고 빈 몸체를 포함한 메타데이터를 검증한다.
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
                    mesh.vbOffset = m[2]; mesh.vbSize = m[0] * 84;
                    mesh.vbDepthOffset = m[3]; mesh.vbDepthSize = m[0] * 3 * layouts[(section - 1) * 2 + 1];
                    mesh.ibOffset = m[4]; mesh.ibSize = m[0] * 12;
                    mesh.draw[0] = { m[0] * 3, 0, 0 };
                    const auto* bounds = metadata + (bins + bin) * 6;
                    Math::Vector3 minimum(BoundsCoordinate(bounds[0]), BoundsCoordinate(bounds[1]), BoundsCoordinate(bounds[2]));
                    Math::Vector3 maximum(BoundsCoordinate(bounds[3]), BoundsCoordinate(bounds[4]), BoundsCoordinate(bounds[5]));
                    for (uint32_t axis = 0; axis < 3; ++axis)
                        valid &= std::isfinite(BoundsCoordinate(bounds[axis])) && std::isfinite(BoundsCoordinate(bounds[axis + 3]))
                            && BoundsCoordinate(bounds[axis]) <= BoundsCoordinate(bounds[axis + 3]);
                    low = Min(low, minimum); high = Max(high, maximum);
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
            m_BodyState = PatchBodyState::Failed;
            WritePatchLogSafely([&error]()
            {
                // 오류 문자열 생성과 파일 기록도 기존 게시 경로에 예외를 전파하지 않는다.
                return std::string("MI3c ERROR: ") + error.what() + "; existing A/B path preserved\n";
            });
        }
        m_BodyJob.reset();
    }

    // 요청 그룹 수의 body×section 압축을 제출하며 빈 몸체의 bin도 보존한다.
    void SkeletalPatchCopy::CompactCutBodies(const std::shared_ptr<const GeometryBuffers> (&inputs)[2], uint32_t bodyCount)
    {
        // 누락된 section도 원래 VB 주소로 대응시켜 재질과 cap을 구별한다.
        DiscardBodyCompaction();
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
        m_BodyJob = std::make_shared<BodyCompactJob>();
        auto& job = *m_BodyJob;
        for (uint32_t side = 0; side < 2; ++side)
        {
            job.Inputs[side] = inputs[side];
            job.Bodies[side] = m_TriangleBodies[side].GetResource();
        }
        job.Templates = templates;
        job.BodyCount = bodies;
        job.Triangles = triangles;
        job.Dropped = uint64_t(m_DroppedCaps[0]) + m_DroppedCaps[1];
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
        struct Constants
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
        auto& context = GraphicsContext::Begin(L"Publish multi skeletal pieces");
        scene.PrepareMeshes(context);
        context.Finish();

        // 다중 경계 대응과 PS4 측정은 아직 지원하지 않음을 명시한다.
        PatchLog("MI4h2 boundary/PS4 measure N/A (multi)\n");

        // rig의 실제 끊긴 관절 수와 topology 증가를 게시 시점에 기록한다.
        SkeletalRigState state;
        auto& skeletal = m_Physics->GetSkeletalPhysics();
        skeletal.ReadDiagnosticState(m_Break.Owner, state);
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
        if (m_Diagnostic)
            PatchLog("SP6 comparison case=%u model=%s mode=%s NodeId=%u name=%s patch/crossing/uncovered=%u/%u/%u GPU==CPU=PASS components/caps/uncapped/error=%zu/%u/%u/%u\n",
                m_DiagnosticCase, m_ModelName.c_str(), m_Mode == SkeletalJointMode::Rig ? "Rig" : m_Mode == SkeletalJointMode::Parent ? "Parent" : "Influence", m_SelectedNode,
                DiagnosticNodeName(m_ModelName, m_SelectedNode).c_str(), m_Result.PatchTriangles, m_Result.CrossingTriangles,
                m_Result.UncoveredCrossing, components, caps, uncapped, error);
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
    }

    // 컴포넌트 제거 상태를 반영해 GPU 선택·병렬 prefix·추출을 제출한다.
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
        for (uint32_t s = 0; s < input.GetSections().size(); ++s)
        {
            const auto& mesh = input.GetMesh(s);
            if (!influence && m_SelectionMasks[s].size() != mesh.numJoints) return false;
            const uint32_t maskOffset = uint32_t(masks.size());
            if (influence)
                masks.insert(masks.end(), input.GetSource()->m_JointIndices.get() + mesh.startJoint,
                    input.GetSource()->m_JointIndices.get() + mesh.startJoint + mesh.numJoints);
            else masks.insert(masks.end(), m_SelectionMasks[s].begin(), m_SelectionMasks[s].end());
            m_PoseOffsets.push_back(posed.GetOffset(input.GetSections()[s].SourceMesh));
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                const auto& draw = mesh.draw[d];
                if (!draw.primCount) continue;
                draws.push_back({ mesh.vbOffset, mesh.vbStride, mesh.ibOffset, mesh.ibFormat == DXGI_FORMAT_R32_UINT,
                    draw.startIndex, draw.baseVertex, draw.primCount / 3, m_PoseOffsets.back(),
                    uint32_t(triangles), maskOffset, mesh.psoFlags, s, d, mesh.numJoints, mesh.vbSize / mesh.vbStride, 0 });
                triangles += draw.primCount / 3;
            }
        }
        if (!triangles || triangles > UINT32_MAX / (3 * 48ull)
            || !input.GetSource()->m_NumNodes || input.GetSource()->m_NumNodes > UINT32_MAX / 8) return false;

        // 대상 전체 삼각형 수를 상한으로 잡아 compaction 출력 overflow를 없앤다.
        m_Input = source.GetGeometry();
        m_Plane = plane; m_Node = node; m_Diagnostic = diagnostic;
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
            if (mesh.numJoints) skinSections[s] = skin++;
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
        c.Plane = plane.WorldPlane; c.LocalToRectangle = plane.WorldToPlane;
        c.LocalToWorld = world;
        c.Epsilon = 1e-5f; c.HalfWidth = plane.HalfWidth; c.HalfHeight = plane.HalfHeight;
        c.NodeCount = input.GetSource()->m_NumNodes;
        c.Influence = influence;
        const auto* triangleMask = source.GetTriangleMask();
        c.HasState = triangleMask != nullptr;
        m_State = triangleMask ? triangleMask->Pending : std::vector<uint32_t>{};
        c.Triangles = uint32_t(triangles); c.Draws = uint32_t(draws.size());
        c.DepthOffset = vertices * 28; c.IndexOffset = vertices * 44;
        geometry.Data.Create(L"Patch packed geometry", vertices * 48, 1);
        geometry.LineageBytes = uint64_t(vertices) * 48;
        Mesh mesh = input.GetMesh(draws.front().SectionId);
        mesh.vbOffset = 0; mesh.vbStride = 28; mesh.vbSize = vertices * 28;
        mesh.vbDepthOffset = c.DepthOffset; mesh.vbDepthSize = vertices * ((mesh.psoFlags & 64) ? 16 : 12);
        mesh.ibOffset = c.IndexOffset; mesh.ibSize = vertices * 4; mesh.ibFormat = DXGI_FORMAT_R32_UINT;
        mesh.meshCBV = 0; mesh.numJoints = 0; mesh.startJoint = 0; mesh.numDraws = 1;
        mesh.psoFlags = (mesh.psoFlags & ~PSOFlags::kHasSkin) | 31;
        mesh.pso = Renderer::GetPSO(mesh.psoFlags);
        mesh.draw[0] = { 0, 0, 0 };
        geometry.Meshes.push_back(mesh);
        alignas(16) const uint32_t metadata[] = { 0, 0, 0, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0, 0, 0, UINT32_MAX, 0, 0 };
        m_Output->LocalBounds.Create(L"Patch counts and local bounds", 11, 4, metadata);
        m_Output->TriangleSources.Create(L"Patch triangle sources", uint32_t(triangles), 12);

        // 삼각형 prefix 뒤에 블록별 합계와 offset을 위한 16바이트 영역을 확보한다.
        const uint32_t blocks = uint32_t((triangles + 63) / 64);
        m_Scan.Create(L"Patch inclusive prefix", uint32_t(triangles + 2 * blocks), 8);

        // draw와 팔레트 입력 및 선택 결과 버퍼를 준비한다.
        m_Descriptors.Create(L"Patch draw inputs", uint32_t(draws.size()), sizeof(DrawInput), draws.data());
        if (masks.empty()) masks.push_back(0);
        m_Masks.Create(L"Patch palette mask", uint32_t(masks.size()), 4, masks.data());
        m_Readback.Create(L"Patch counts and bounds readback", 12, 4);
        const std::vector<uint32_t> votes(size_t(c.NodeCount) * 2, 0);
        m_Votes.Create(L"Patch uint64 joint votes", c.NodeCount, 8, votes.data());
        alignas(16) const uint32_t selectedNode[4] = { node, 0, 0, 0 };
        m_Selected.Create(L"Patch selected NodeId", 1, 4, selectedNode);

        // 마스크가 없으면 State SRV에 바인딩할 0 버퍼를 준비한다.
        const std::vector<uint32_t> emptyState(triangleMask ? 4 : (size_t(triangles) + 3) & ~size_t(3), 0);
        m_EmptyState.Create(L"Empty triangle state", uint32_t(emptyState.size()), 4, emptyState.data());

        // 같은 graphics queue에서 pose 준비 뒤 추출하고 패스 사이 UAV 의존성을 보장한다.
        auto& graphics = GraphicsContext::Begin(L"Extract skeletal patch");
        auto& compute = graphics.GetComputeContext();
        if (triangleMask) const_cast<MeshComponent&>(source).UpdateTriangleMask(graphics);
        compute.SetRootSignature(PatchRoot); compute.SetPipelineState(PatchPSO);
        auto& inputData = const_cast<ByteAddressBuffer&>(input.GetDataBuffer());
        auto& inputPose = const_cast<ByteAddressBuffer&>(posed.GetBuffer());
        compute.TransitionResource(inputData, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(inputPose, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(m_Descriptors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(m_Masks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.SetBufferSRV(1, input.GetDataBuffer()); compute.SetBufferSRV(2, posed.GetBuffer());
        compute.SetBufferSRV(3, m_Descriptors); compute.SetBufferSRV(4, m_Masks);
        auto& state = triangleMask ? const_cast<StructuredBuffer&>(triangleMask->State) : m_EmptyState;
        compute.TransitionResource(state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.SetBufferSRV(11, state);
        GpuBuffer* outputs[] = { &geometry.Data, &m_Output->TriangleSources, &m_Output->LocalBounds, &m_Scan, &m_Votes, &m_Selected };
        for (uint32_t i = 0; i < 6; ++i)
        {
            compute.TransitionResource(*outputs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.SetBufferUAV(i + 5, *outputs[i]);
        }
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

        // 블록 scan·블록 offset scan·offset 적용을 거친 뒤 삼각형을 추출한다.
        const uint32_t modes[] = { 0, 1, 6, 7, 2 };
        for (uint32_t mode : modes)
        {
            // 블록 합계 scan만 한 그룹을 쓰고 나머지는 기존 2D 분할을 유지한다.
            c.Mode = mode;
            compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
            if (c.Mode == 6) compute.Dispatch(1);
            else compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);

            // 다음 패스가 읽을 scan과 기존 bounds 초기화의 UAV 의존성을 보장한다.
            if (c.Mode != 2)
            {
                compute.InsertUAVBarrier(m_Scan);
                compute.InsertUAVBarrier(m_Output->LocalBounds);
            }
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
        m_Fence = graphics.Finish();
        return true;
    }

    // fence 완료 뒤 카운터·경계와 GPU 출력을 수집한다.
    bool SkeletalPatchJob::Poll(SkeletalPatchResult& result, std::shared_ptr<SkeletalPatchBuffers>& patch)
    {
        // GPU가 아직 쓰는 readback은 매핑하지 않는다.
        if (!m_Fence || !Graphics::g_CommandManager.IsFenceComplete(m_Fence))
            return false;

        // 기본 경로는 정점 대신 세 카운터와 로컬 경계·원본 월드 X 경계만 읽는다.
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
        PatchLog("Skeletal patch GPU patch=%u crossing=%u uncovered=%u\n", result.PatchTriangles,
            result.CrossingTriangles, result.UncoveredCrossing);

        // section과 draw 출처를 원본 mask의 누적 삼각형 번호로 변환한다.
        const auto* origins = static_cast<const uint32_t*>(m_OriginsReadback.Map());
        for (uint32_t t = 0; t < result.PatchTriangles; ++t)
        {
            uint32_t first = 0;
            for (uint32_t s = 0; s <= origins[t * 3]; ++s)
                for (uint32_t d = 0; d < m_Input->GetMesh(s).numDraws; ++d)
                    if (s < origins[t * 3] || d < origins[t * 3 + 1]) first += m_Input->GetMesh(s).draw[d].primCount / 3;
            m_Output->Triangles.push_back(first + origins[t * 3 + 2]);
            // draw별 번호를 MI-1이 보존한 skin section 연속 삼각형 번호로 바꾼다.
            uint32_t local = origins[t * 3 + 2];
            for (uint32_t d = 0; d < origins[t * 3 + 1]; ++d)
                local += m_Input->GetMesh(origins[t * 3]).draw[d].primCount / 3;
            m_Output->IslandSources.push_back({ m_SkinSections.at(origins[t * 3]), local });
        }
        m_OriginsReadback.Unmap();
        if (m_Diagnostic) Validate(result);
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
        mesh.vbSize = vertices * 28; mesh.ibSize = vertices * 4;
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

    // 제출 포즈를 CPU 스키닝해 실제 절단 평면의 부호와 아일랜드 다수결 근거를 남긴다.
    void SkeletalPatchJob::DiagnoseIslands(SkeletalPatchBuffers& patch, const MeshCutting::CutPlaneData& plane,
        float offset, bool positiveA, const std::string& modelName)
    {
        // skin section별 제출 프레임을 찾고 아직 계산하지 않은 부호를 표시한다.
        const auto& model = *m_Input->GetSource();
        std::vector<std::vector<int8_t>> signs(model.m_SkinCpuSections.size());
        std::vector<uint32_t> frames(signs.size(), UINT32_MAX);
        for (size_t s = 0; s < m_SkinSections.size(); ++s)
        {
            const uint32_t skin = m_SkinSections[s];
            if (skin == UINT32_MAX) continue;
            frames.at(skin) = uint32_t(s);
        }

        // 패치 삼각형이 참조하는 정점만 중복 없이 PreparePoseCS와 같은 방식으로 스키닝한다.
        for (const auto& source : patch.IslandSources)
        {
            const uint32_t skin = source[0];
            const auto& section = model.m_SkinCpuSections.at(skin);
            if (signs[skin].empty()) signs[skin].resize(section.size(), 2);
            for (uint32_t k = 0; k < 3; ++k)
            {
                const uint32_t v = model.m_LatticeTopology.Indices.at(skin).at(source[1] * 3 + k);
                if (signs[skin][v] != 2) continue;
                const auto& vertex = section.at(v);
                Math::Vector4 skinned(Math::kZero);
                uint32_t sum = 0;
                for (uint32_t weight : vertex.Weights) sum += weight;
                for (uint32_t j = 0; j < 4; ++j)
                    if (vertex.Weights[j]) skinned = skinned + (m_IslandJoints.at(section.StartJoint + vertex.Joints[j]).posXform
                        * Math::Vector4(vertex.Position[0], vertex.Position[1], vertex.Position[2], 1))
                        * (float(vertex.Weights[j]) / std::max(sum, 1u));
                const Math::Vector3 local(m_IslandFrames.at(frames.at(skin)) * skinned);
                const Math::Vector3 world(m_SourceWorld * Math::Vector4(local, 1));
                const float distance = Math::Dot(plane.WorldPlane,
                    Math::Vector4(world + Math::Vector3(offset, 0, 0), 1));
                signs[skin][v] = std::abs(distance) <= 1e-5f ? 0 : distance > 0 ? 1 : -1;
            }
        }

        // 요청당 한 번 공통 분류기로 팔레트 joint의 몸체 그룹을 읽는다.
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
                if (!IsSelectedSegmentChild(snapshot.Root, child, m_Parents[child], m_CutChildren[i])) continue;

                // 제출 포즈의 선택 선분과 평면 사이의 교점을 계산한다.
                const Math::Vector3 position(frame * m_IslandNodes[child].GetW());
                const float b = Dot(plane.WorldPlane, Math::Vector4(position + Math::Vector3(offset, 0, 0), 1));
                float fraction = 0;
                uint32_t side = 0;
                if (!IntersectLatticeSegment(a, b, fraction, side)) continue;
                const Math::Vector3 crossing = parent + (position - parent) * fraction + Math::Vector3(offset, 0, 0);
                const float distance = LengthSquare(crossing - center);
                if (distance >= closest) continue;

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
        patch.Islands = m_IslandLabels = LabelPatchIslands(model.m_LatticeTopology, model.m_SkinCpuSections,
            patch.IslandSources, signs, group);
        uint32_t reassigned = 0;
        for (const auto& island : patch.Islands.Islands)
            reassigned += island.Body != uint32_t(island.Side == 0 ? !positiveA : positiveA);
        PatchLog("MI2 islands=%u reassigned=%u side0Default=%u side1Default=%u\n", patch.Islands.IslandCount,
            reassigned, patch.Islands.SideDefault[0], patch.Islands.SideDefault[1]);

        // 최대 기여 joint 이름과 점수로 허리 등 떨어진 아일랜드의 소속 근거를 보인다.
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
        uint32_t crossing = 0, uncovered = 0, infiniteCrossing = 0;
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
        // 제거 삼각형은 선택 여부뿐 아니라 교차·미포함 카운터에서도 제외한다.
        size_t selected = 0;
        uint32_t global = 0;
        for (uint32_t s = 0; s < input.Sections.size(); ++s)
            for (uint32_t d = 0; d < input.Sections[s].Draws.size(); ++d)
                for (uint32_t t = 0; t < input.Sections[s].Draws[d].IndexCount / 3; ++t, ++global)
                {
                    if (!input.State.empty() && (input.State[global] & 0x80000000u)) continue;
                    const bool include = selected < expected.size() && expected[selected].SectionId == s
                        && expected[selected].DrawId == d && expected[selected].TriangleId == t;
                    if (include) ++selected;
                    const auto indices = TriangleIndices(input.Sections[s], input.Sections[s].Draws[d], t);
                    float minimum = FLT_MAX, maximum = -FLT_MAX;
                    for (uint32_t index : indices)
                    {
                        const float distance = float(Math::Dot(Math::Vector4(input.Sections[s].Vertices[index].Position, 1), input.Plane.WorldPlane));
                        minimum = std::min(minimum, distance); maximum = std::max(maximum, distance);
                    }
                    if (minimum < -input.Epsilon && maximum > input.Epsilon) ++infiniteCrossing;
                    if (CrossesRectangle(input, input.Sections[s], indices))
                    {
                        ++crossing;
                        if (!include) ++uncovered;
                    }
                }
        pass &= crossing == result.CrossingTriangles && uncovered == result.UncoveredCrossing;
        PatchLog("Skeletal patch diagnostic infinite-plane crossings=%u\n", infiniteCrossing);
        PatchLog("Skeletal patch CPU patch=%zu crossing=%u uncovered=%u sources=%s %s\n", expected.size(),
            crossing, uncovered, pass ? "match" : "mismatch", pass ? "PASS" : "FAIL");
        if (!pass) throw std::runtime_error("Skeletal patch GPU/CPU diagnostic FAIL");
        PatchLog("MI4b GPU==CPU PASS triangles=%zu duplicates=%zu\n", expected.size(), duplicates);
    }
}
