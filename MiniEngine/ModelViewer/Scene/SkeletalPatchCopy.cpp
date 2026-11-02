#include "SkeletalPatchCopy.h"
#include "CommandListManager.h"
#include "Renderer.h"
#include <cstring>

namespace Viewer
{
    // 현재 모델 공간 joint 원점을 GPU pose와 같은 월드로 옮기고 section별 팔레트를 모은다.
    SegmentJointSelection SelectSegmentJoint(const ModelInstance& pose, const MeshComponent& component,
        const SceneTransform& owner, const MeshCutting::CutPlaneData& plane)
    {
        // 유효 pose의 위치만 읽으며 정점 readback은 하지 않는다.
        if (!pose.HasValidPose() || component.GetDeformBinding().Pose != PoseMode::FollowModel)
            return {};
        const auto& model = *pose.GetModel();
        const Math::Matrix4 modelToWorld = Math::Matrix4(component.GetWorldTransform(owner))
            * Invert(Math::Matrix4(component.GetDeformBinding().SourceFrame.ToAffine()));
        std::vector<Math::Vector3> positions(model.m_NumNodes);
        for (uint32_t node = 0; node < model.m_NumNodes; ++node)
            positions[node] = Math::Vector3(modelToWorld * Math::Vector4(Math::Vector3(pose.GetNodeTransforms()[node].GetW()), 1));

        // 이 컴포넌트가 참조하는 mesh의 skin joint만 선택 집합에 포함한다.
        std::vector<std::vector<uint32_t>> palettes;
        const auto& geometry = *component.GetGeometry();
        for (size_t section = 0; section < geometry.GetSections().size(); ++section)
        {
            const auto& mesh = geometry.GetMesh(section);
            palettes.emplace_back(model.m_JointIndices.get() + mesh.startJoint,
                model.m_JointIndices.get() + mesh.startJoint + mesh.numJoints);
        }
        return SelectSegmentJoint(pose.GetPoseParents(), positions, palettes, plane);
    }

    // 유한 평면 사각형에 닿는 skin 경계 구 중 중심이 가장 가까운 대상을 고른다.
    void LogSkeletalPatchSelection(const Scene& scene, const MeshCutting::CutPlaneData& plane)
    {
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

        // 선택 실패와 compute pose 거절을 구분하고 SP-3 작업은 아직 제출하지 않는다.
        if (!target || !Renderer::UseComputePose)
        {
            Utility::Printf("Skeletal patch rejected: %s\n", !target ? "no skin target" : "compute pose disabled");
            return;
        }
        const auto selected = SelectSegmentJoint(*target->GetModelInstance(), *mesh, target->GetTransform(), plane);
        if (selected.NodeId == UINT32_MAX)
        {
            Utility::Printf("Skeletal patch rejected: no current skin joint pose (frozen or invalid input)\n");
            return;
        }

        // 현재 GPU 입력과 일치하는 유효 선택만 segment 또는 말단으로 기록한다.
        Utility::Printf("Skeletal patch target=%s NodeId=%u kind=%s child=%u intersection=(%.6f, %.6f, %.6f)\n",
            target->GetName().c_str(), selected.NodeId, selected.Kind == SegmentJointKind::Segment ? "segment" : "leaf",
            selected.ChildNodeId, float(selected.Intersection.GetX()), float(selected.Intersection.GetY()), float(selected.Intersection.GetZ()));
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
            const auto world = object->GetTransform().ToAffine();
            const Vector3 a = world * Vector3(pose.GetNodeTransforms()[nodes[index]].GetW());
            const Vector3 b = world * Vector3(pose.GetNodeTransforms()[index == 2 ? 11 : nodes[index] + 1].GetW());
            const Vector3 normal = Normalize(a - b);
            const Vector3 x = Normalize(Cross(normal, std::abs(float(normal.GetY())) < 0.9f ? Vector3(kYUnitVector) : Vector3(kXUnitVector)));
            const Vector3 origin = index == 2 ? a + (a - b) * 0.2f : (a + b) * 0.5f;
            Utility::Printf("Skeletal patch diagnostic %s\n", index == 0 ? "upper arm" : index == 1 ? "thigh" : "hand");
            return MeshCutting::TryBuildCutPlaneData({ 0.3f, 0.3f }, AffineTransform(Matrix3(x, normal, Cross(x, normal)), origin), plane);
        }
        return false;
    }

    // 완료 전 작업 자원을 파괴하지 않도록 제출 fence를 기다린다.
    SkeletalPatchJob::~SkeletalPatchJob()
    {
        // 제출되지 않은 작업은 GPU 대기가 필요 없다.
        if (m_Fence != 0)
            Graphics::g_CommandManager.WaitForFence(m_Fence);
    }

    // SP-3 추출 패스가 연결되기 전에는 GPU 작업을 제출하지 않는다.
    bool SkeletalPatchJob::Submit(const MeshComponent&, const MeshCutting::CutPlaneData&)
    {
        // 투표 패스 제거 후 제출·fence·readback 수명 골격만 보존한다.
        return false;
    }

    // fence 완료 여부만 확인하고 대기 없이 작은 결과를 수집한다.
    bool SkeletalPatchJob::Poll(SkeletalPatchResult& result, std::shared_ptr<GeometryBuffers>& patch)
    {
        // GPU가 아직 쓰는 readback은 매핑하지 않는다.
        if (!m_Fence || !Graphics::g_CommandManager.IsFenceComplete(m_Fence))
            return false;

        // SP-2에는 추출 버퍼가 없으며 세 DWORD만 반환한다.
        std::memcpy(&result, m_Readback.Map(), sizeof(result));
        m_Readback.Unmap();
        patch.reset();
        return true;
    }
}
