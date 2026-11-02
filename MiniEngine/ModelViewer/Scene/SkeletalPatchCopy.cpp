#include "SkeletalPatchCopy.h"
#include "CommandListManager.h"
#include "Renderer.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "CompiledShaders/PatchExtractCS.h"
#include "tests/SkeletalPatchReference.h"
#include <cstdio>
#include <cstdarg>
#include <stdexcept>
#include <cstring>

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
            uint32_t DrawId, JointCount, Reserved0, Reserved1;
        };
        struct Parameters
        {
            Math::Vector4 Plane;
            Math::Matrix4 LocalToRectangle;
            float Epsilon, HalfWidth, HalfHeight;
            uint32_t Triangles, Draws, DepthOffset, IndexOffset, Mode;
        };
        static_assert(sizeof(DrawInput) == 64 && sizeof(Parameters) == 112, "Patch shader layout");
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
            Utility::Printf("%s", line);
            FILE* file = nullptr;
            if (fopen_s(&file, "SkeletalPatchChecks.log", "a") == 0)
            {
                fputs(line, file);
                fclose(file);
            }
        }

        // 세 추출 패스가 공유하는 root signature와 compute PSO를 준비한다.
        void InitializePatch()
        {
            // 여러 C 요청 사이에 파이프라인을 재사용한다.
            static bool initialized = false;
            if (initialized) return;
            PatchRoot.Reset(10);
            PatchRoot[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 4; ++i) PatchRoot[i + 1].InitAsBufferSRV(i);
            for (uint32_t i = 0; i < 5; ++i) PatchRoot[i + 5].InitAsBufferUAV(i);
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
        return SelectSegmentJoint(pose.GetPoseParents(), positions, palettes, plane);
    }

    // 유한 평면 사각형에 닿는 가장 가까운 skin 대상을 선택하고 추출한다.
    void RequestSkeletalPatch(const Scene& scene, const MeshCutting::CutPlaneData& plane,
        SkeletalPatchJob& job, bool diagnostic)
    {
        // 새 C는 대상 유무와 관계없이 이전 결과를 무효화한다.
        job.Cancel();

        // 평면 로컬에서 가장 가까운 사각형 점을 월드로 돌려 경계 구와 비교한다.
        using namespace Math;
        const Vector3 center = (plane.WorldCorners[0] + plane.WorldCorners[2]) * 0.5f;
        const Matrix4 planeToWorld = Invert(plane.WorldToPlane);
        const GameObject* target = nullptr;
        const MeshComponent* mesh = nullptr;
        float nearest = FLT_MAX;
        for (const auto& object : scene.GetObjects())
            for (const auto& component : object->GetMeshComponents())
            {
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
                if (float(LengthSquare(bounds.GetCenter() - closest)) > float(bounds.GetRadius() * bounds.GetRadius()) || distance >= nearest)
                    continue;
                nearest = distance;
                target = object.get();
                mesh = component.get();
            }

        // 선택 실패와 compute pose 거절을 구분한다.
        if (!target || !Renderer::UseComputePose)
        {
            PatchLog("Skeletal patch rejected: %s\n", !target ? "no skin target" : "compute pose disabled");
            if (diagnostic) throw std::runtime_error("Skeletal patch diagnostic target rejected");
            return;
        }
        const auto selected = SelectSegmentJoint(*target->GetModelInstance(), *mesh, target->GetTransform(), plane);
        if (selected.NodeId == UINT32_MAX)
        {
            PatchLog("Skeletal patch rejected: no current skin joint pose (frozen or invalid input)\n");
            if (diagnostic) throw std::runtime_error("Skeletal patch diagnostic selection rejected");
            return;
        }

        // 현재 GPU 입력과 일치하는 유효 선택만 segment 또는 말단으로 기록한다.
        PatchLog("Skeletal patch target=%s NodeId=%u kind=%s child=%u intersection=(%.6f, %.6f, %.6f)\n",
            target->GetName().c_str(), selected.NodeId, selected.Kind == SegmentJointKind::Segment ? "segment" : "leaf",
            selected.ChildNodeId, float(selected.Intersection.GetX()), float(selected.Intersection.GetY()), float(selected.Intersection.GetZ()));

        // mini 캐시는 이름을 보관하지 않으므로 진단 자산의 glb DFS 이름표를 사용한다.
        if (diagnostic && target->GetName() == "CesiumMan")
        {
            const char* names[] = { "Z_UP", "Armature", "Skeleton_torso_joint_1", "Skeleton_torso_joint_2",
                "torso_joint_3", "Skeleton_neck_joint_1", "Skeleton_neck_joint_2", "Skeleton_arm_joint_L__4_",
                "Skeleton_arm_joint_L__3_", "Skeleton_arm_joint_L__2_", "Skeleton_arm_joint_R",
                "Skeleton_arm_joint_R__2_", "Skeleton_arm_joint_R__3_", "leg_joint_L_1", "leg_joint_L_2",
                "leg_joint_L_3", "leg_joint_L_5", "leg_joint_R_1", "leg_joint_R_2", "leg_joint_R_3", "leg_joint_R_5", "Cesium_Man" };
            PatchLog("Skeletal patch selected NodeId=%u name=%s kind=%s child=%u\n", selected.NodeId,
                selected.NodeId < _countof(names) ? names[selected.NodeId] : "unknown",
                selected.Kind == SegmentJointKind::Segment ? "segment" : "leaf", selected.ChildNodeId);
        }

        // cutter와 같은 component-local 평면 변환을 사용한다.
        const Matrix4 world(mesh->GetWorldTransform(target->GetTransform()));
        auto local = plane;
        local.WorldPlane = Transpose(world) * plane.WorldPlane;
        local.WorldToPlane = plane.WorldToPlane * world;
        if (!job.Submit(*mesh, local, selected.PaletteMasks, selected.NodeId, diagnostic) && diagnostic)
            throw std::runtime_error("Skeletal patch diagnostic submit rejected");
    }

    // 변환기의 DFS NodeId로 현재 위팔·허벅지 segment와 손 말단의 평면을 만든다.
    bool BuildSkeletalPatchDiagnosticPlane(const Scene& scene, uint32_t index, MeshCutting::CutPlaneData& plane)
    {
        // CesiumMan.glb의 오른쪽 위팔(10→11), 허벅지(17→18), 손(12)을 사용한다.
        using namespace Math;
        const uint32_t nodes[] = { 10, 17, 12 };
        for (const auto& object : scene.GetObjects())
        {
            if (object->GetName() != "CesiumMan" || index >= 3)
                continue;
            const auto& pose = *object->GetModelInstance();
            if (!pose.HasValidPose() || pose.GetModel()->m_NumNodes <= 18)
                return false;
            const auto world = JointFrameToWorld(pose, *object->GetMeshComponents().front(), object->GetTransform(), 0);
            const Vector3 a = Vector3(world * Vector4(Vector3(pose.GetNodeTransforms()[nodes[index]].GetW()), 1));
            const Vector3 b = Vector3(world * Vector4(Vector3(pose.GetNodeTransforms()[index == 2 ? 11 : nodes[index] + 1].GetW()), 1));
            const Vector3 normal = Normalize(a - b);
            const Vector3 x = Normalize(Cross(normal, std::abs(float(normal.GetY())) < 0.9f ? Vector3(kYUnitVector) : Vector3(kXUnitVector)));
            const Vector3 origin = index == 2 ? a + (a - b) * 0.2f : (a + b) * 0.5f;
            PatchLog("Skeletal patch diagnostic %s\n", index == 0 ? "upper arm" : index == 1 ? "thigh" : "hand");
            return MeshCutting::TryBuildCutPlaneData({ 0.3f, 0.3f }, AffineTransform(Matrix3(x, normal, Cross(x, normal)), origin), plane);
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
    }

    // 원본 GPU 입력에서 분류·prefix sum·packed 출력을 순서대로 제출한다.
    bool SkeletalPatchJob::Submit(const MeshComponent& source, const MeshCutting::CutPlaneData& plane,
        const std::vector<std::vector<uint32_t>>& mask, uint32_t node, bool diagnostic)
    {
        // 준비되지 않았거나 생성된 입력은 이 단계의 skeletal 추출 대상이 아니다.
        Cancel();
        const auto& input = *source.GetGeometry();
        const auto& posed = source.GetPosedGeometry();
        if (!Renderer::UseComputePose || !posed.IsReady() || input.IsGenerated()
            || mask.size() != input.GetSections().size()) return false;
        InitializePatch();
        std::vector<DrawInput> draws;
        std::vector<uint32_t> masks;
        uint64_t triangles = 0;
        for (uint32_t s = 0; s < input.GetSections().size(); ++s)
        {
            const auto& mesh = input.GetMesh(s);
            if (mask[s].size() != mesh.numJoints) return false;
            const uint32_t maskOffset = uint32_t(masks.size());
            masks.insert(masks.end(), mask[s].begin(), mask[s].end());
            m_PoseOffsets.push_back(posed.GetOffset(input.GetSections()[s].SourceMesh));
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                const auto& draw = mesh.draw[d];
                if (!draw.primCount) continue;
                draws.push_back({ mesh.vbOffset, mesh.vbStride, mesh.ibOffset, mesh.ibFormat == DXGI_FORMAT_R32_UINT,
                    draw.startIndex, draw.baseVertex, draw.primCount / 3, m_PoseOffsets.back(),
                    uint32_t(triangles), maskOffset, mesh.psoFlags, s, d, mesh.numJoints, 0, 0 });
                triangles += draw.primCount / 3;
            }
        }
        if (!triangles || triangles > UINT32_MAX / (3 * 48ull)) return false;

        // 대상 전체 삼각형 수를 상한으로 잡아 compaction 출력 overflow를 없앤다.
        m_Input = source.GetGeometry();
        m_Plane = plane; m_Node = node; m_Diagnostic = diagnostic;
        m_Output = std::make_shared<SkeletalPatchBuffers>();
        m_Output->Geometry = std::make_shared<GeometryBuffers>();
        auto& geometry = *m_Output->Geometry;
        geometry.Source = input.GetSource();
        const uint32_t vertices = uint32_t(triangles) * 3;
        Parameters c{};
        c.Plane = plane.WorldPlane; c.LocalToRectangle = plane.WorldToPlane;
        c.Epsilon = 1e-5f; c.HalfWidth = plane.HalfWidth; c.HalfHeight = plane.HalfHeight;
        c.Triangles = uint32_t(triangles); c.Draws = uint32_t(draws.size());
        c.DepthOffset = vertices * 28; c.IndexOffset = vertices * 44;
        geometry.Data.Create(L"Patch packed geometry", vertices * 48, 1);
        geometry.Pose.Create(L"Patch RAW40 pose", vertices * 40, 1);
        geometry.PoseOffsets.push_back(0);
        Mesh mesh = input.GetMesh(draws.front().SectionId);
        mesh.vbOffset = 0; mesh.vbStride = 28; mesh.vbSize = vertices * 28;
        mesh.vbDepthOffset = c.DepthOffset; mesh.vbDepthSize = vertices * ((mesh.psoFlags & 64) ? 16 : 12);
        mesh.ibOffset = c.IndexOffset; mesh.ibSize = vertices * 4; mesh.ibFormat = DXGI_FORMAT_R32_UINT;
        mesh.meshCBV = 0; mesh.numJoints = 0; mesh.startJoint = 0; mesh.numDraws = 1;
        mesh.psoFlags = (mesh.psoFlags & ~PSOFlags::kHasSkin) | 31;
        mesh.pso = Renderer::GetPSO(mesh.psoFlags);
        mesh.draw[0] = { 0, 0, 0 };
        geometry.Meshes.push_back(mesh);
        const uint32_t metadata[] = { 0, 0, 0, UINT32_MAX, UINT32_MAX, UINT32_MAX, 0, 0, 0 };
        m_Output->LocalBounds.Create(L"Patch counts and local bounds", 9, 4, metadata);
        m_Output->TriangleSources.Create(L"Patch triangle sources", uint32_t(triangles), 12);
        m_Scan.Create(L"Patch inclusive prefix", uint32_t(triangles), 8);
        m_Descriptors.Create(L"Patch draw inputs", uint32_t(draws.size()), sizeof(DrawInput), draws.data());
        if (masks.empty()) masks.push_back(0);
        m_Masks.Create(L"Patch palette mask", uint32_t(masks.size()), 4, masks.data());
        m_Readback.Create(L"Patch counts and bounds readback", 9, 4);

        // 같은 graphics queue에서 pose 준비 뒤 추출하고 패스 사이 UAV 의존성을 보장한다.
        auto& graphics = GraphicsContext::Begin(L"Extract skeletal patch");
        auto& compute = graphics.GetComputeContext();
        compute.SetRootSignature(PatchRoot); compute.SetPipelineState(PatchPSO);
        auto& inputData = const_cast<ByteAddressBuffer&>(input.GetDataBuffer());
        auto& inputPose = const_cast<ByteAddressBuffer&>(posed.GetBuffer());
        compute.TransitionResource(inputData, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(inputPose, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(m_Descriptors, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(m_Masks, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.SetBufferSRV(1, input.GetDataBuffer()); compute.SetBufferSRV(2, posed.GetBuffer());
        compute.SetBufferSRV(3, m_Descriptors); compute.SetBufferSRV(4, m_Masks);
        GpuBuffer* outputs[] = { &geometry.Data, &geometry.Pose, &m_Output->TriangleSources, &m_Output->LocalBounds, &m_Scan };
        for (uint32_t i = 0; i < 5; ++i)
        {
            compute.TransitionResource(*outputs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            compute.SetBufferUAV(i + 5, *outputs[i]);
        }
        const uint32_t groups = (c.Triangles + 63) / 64;
        for (c.Mode = 0; c.Mode < 3; ++c.Mode)
        {
            compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
            compute.Dispatch(c.Mode == 1 ? 1 : std::min(groups, 65535u), c.Mode == 1 ? 1 : (groups + 65534) / 65535);
            if (c.Mode < 2)
            {
                // 다음 패스가 읽을 UAV만 동기화하고 마지막 패스는 상태 전환으로 마친다.
                compute.InsertUAVBarrier(m_Scan);
                compute.InsertUAVBarrier(m_Output->LocalBounds);
            }
        }
        graphics.TransitionResource(m_Output->LocalBounds, D3D12_RESOURCE_STATE_COPY_SOURCE);
        graphics.CopyBufferRegion(m_Readback, 0, m_Output->LocalBounds, 0, 36);

        // 진단에서만 동일 제출 시점의 VB/IB·pose·출처를 저장한다.
        if (diagnostic)
        {
            m_SourceReadback.Create(L"Patch diagnostic source", uint32_t(input.GetDataBuffer().GetBufferSize()), 1);
            m_PoseReadback.Create(L"Patch diagnostic pose", uint32_t(posed.GetBuffer().GetBufferSize()), 1);
            m_OriginsReadback.Create(L"Patch diagnostic sources", uint32_t(triangles), 12);
            graphics.CopyBuffer(m_SourceReadback, inputData);
            graphics.CopyBuffer(m_PoseReadback, inputPose);
            graphics.CopyBuffer(m_OriginsReadback, m_Output->TriangleSources);
        }
        graphics.TransitionResource(inputData, D3D12_RESOURCE_STATE_GENERIC_READ);
        graphics.TransitionResource(inputPose, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        graphics.TransitionResource(geometry.Data, D3D12_RESOURCE_STATE_GENERIC_READ);
        graphics.TransitionResource(geometry.Pose, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        graphics.TransitionResource(m_Output->TriangleSources, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        graphics.TransitionResource(m_Output->LocalBounds, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        m_Fence = graphics.Finish();
        return true;
    }

    // fence 완료 여부만 확인하고 대기 없이 카운터·경계와 GPU 버퍼를 수집한다.
    bool SkeletalPatchJob::Poll(SkeletalPatchResult& result, std::shared_ptr<SkeletalPatchBuffers>& patch)
    {
        // GPU가 아직 쓰는 readback은 매핑하지 않는다.
        if (!m_Fence || !Graphics::g_CommandManager.IsFenceComplete(m_Fence))
            return false;

        // 기본 경로는 정점 대신 세 카운터와 여섯 경계 좌표만 읽는다.
        uint32_t metadata[9];
        std::memcpy(metadata, m_Readback.Map(), sizeof(metadata));
        std::memcpy(&result, metadata, sizeof(result));
        m_Readback.Unmap();
        PatchLog("Skeletal patch GPU patch=%u crossing=%u uncovered=%u\n", result.PatchTriangles,
            result.CrossingTriangles, result.UncoveredCrossing);
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
        patch = std::move(m_Output);
        m_Fence = 0;
        return true;
    }

    // 제출 프레임의 GPU 입력을 CPU 기준으로 복원하여 개수와 압축 출처 순서를 검사한다.
    void SkeletalPatchJob::Validate(const SkeletalPatchResult& result)
    {
        // GPU가 완료된 진단 snapshot만 읽고 평상시 CPU 추출은 실행하지 않는다.
        using namespace PatchReference;
        Input input;
        input.Plane = m_Plane;
        const auto& model = *m_Input->GetSource();
        input.JointNodes.assign(model.m_JointIndices.get(), model.m_JointIndices.get() + model.m_NumJoints);
        const auto* data = static_cast<const uint8_t*>(m_SourceReadback.Map());
        const auto* pose = static_cast<const uint8_t*>(m_PoseReadback.Map());
        for (uint32_t s = 0; s < m_Input->GetSections().size(); ++s)
        {
            const auto& mesh = m_Input->GetMesh(s);
            Section section;
            section.StartJoint = mesh.startJoint;
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
        const auto expected = ExtractPatchReference(input, m_Node);
        uint32_t crossing = 0, uncovered = 0, infiniteCrossing = 0;
        const auto* origins = static_cast<const uint32_t*>(m_OriginsReadback.Map());
        bool pass = expected.size() == result.PatchTriangles;
        for (uint32_t i = 0; i < expected.size() && i < result.PatchTriangles; ++i)
            pass &= origins[i * 3] == expected[i].SectionId && origins[i * 3 + 1] == expected[i].DrawId
                && origins[i * 3 + 2] == expected[i].TriangleId;
        m_OriginsReadback.Unmap();
        size_t selected = 0;
        for (uint32_t s = 0; s < input.Sections.size(); ++s)
            for (uint32_t d = 0; d < input.Sections[s].Draws.size(); ++d)
                for (uint32_t t = 0; t < input.Sections[s].Draws[d].IndexCount / 3; ++t)
                {
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
    }
}
