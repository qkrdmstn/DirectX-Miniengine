#include "ObjectSelection.h"
#include "GizmoMath.h"
#include "PhysicsSystem.h"
#include "CuttingSystem.h"
#include <cmath>
#include <limits>

namespace Viewer
{
    // 절단과 물리 참조를 먼저 정리하고 대상 종류에 맞게 소유물을 삭제한다.
    bool DeleteEditTarget(Scene& scene, PhysicsSystem* physics, CuttingSystem* cutting, const EditTarget& target)
    {
        // a. 대상 유효성과 owner 편집 잠금을 먼저 검사한다.
        if (!IsEditTargetValid(scene, target)) return false;
        GameObject* object = scene.FindObject(target.Object);
        if (!object->IsEditable()) return false;
        const ComponentId mesh = target.IsMesh() ? target.Mesh : InvalidComponentId;

        // b. 삭제될 대상의 절단 대기 후보를 제거한다.
        if (cutting) cutting->CancelTarget(target.Object, mesh);

        // c. 다음 step과 query 전에 물리 매핑을 제거한다.
        if (physics) physics->RemoveTarget(target.Object, mesh);

        // d. 조각 삭제는 owner를 유지하고 owner 삭제는 객체 전체를 제거한다.
        return target.IsMesh() ? object->RemoveMeshComponent(mesh) : scene.DeleteObject(target.Object);
    }

    // 대상 유효성과 기록한 변환 모드를 확인하고 이전 월드 TRS를 복구한다.
    bool ApplyHistoryCommand(Scene& scene, PhysicsSystem* physics, const TransformHistory::Command& command)
    {
        // 삭제되거나 대상 종류에 맞지 않는 명령을 건너뛴다.
        if (!IsEditTargetValid(scene, command.Target)) return false;

        // Mesh의 현재 모드가 기록 당시와 다르면 적용하지 않는다.
        if (command.Target.IsMesh())
        {
            const auto* mesh = scene.FindObject(command.Target.Object)->FindMeshComponent(command.Target.Mesh);
            const HistoryTransformMode mode = mesh->GetTransformMode() == MeshTransformMode::Attached ?
                HistoryTransformMode::Attached : HistoryTransformMode::WorldIndependent;
            if (mode != command.Mode) return false;
        }

        // 물리 이동 이후에도 After 비교 없이 트랜잭션 시작 상태를 복구한다.
        return ApplyEditTransform(scene, physics, command.Target, command.Before);
    }

    // Owner는 기존 경로로, 독립 Mesh는 물리 갱신 후 렌더 변환으로 적용한다.
    bool ApplyEditTransform(Scene& scene, PhysicsSystem* physics, const EditTarget& target, const SceneTransform& world)
    {
        // 대상의 편집 가능 여부와 정규화된 월드 TRS를 먼저 검사한다.
        GameObject* object = scene.FindObject(target.Object);
        if (!object || !object->IsEditable()) return false;
        if (target.Kind == EditTargetKind::Owner) return object->SetTransform(world);
        MeshComponent* mesh = object->FindMeshComponent(target.Mesh);
        SceneTransform normalized = world;
        if (!mesh || mesh->GetTransformMode() != MeshTransformMode::WorldIndependent || !normalized.Normalize()) return false;

        // 물리 갱신이 성공한 경우에만 렌더 변환과 편집 세대를 게시한다.
        if (physics) return physics->ApplyMeshEdit(scene, target.Object, target.Mesh, normalized);
        return mesh->SetTransform(normalized, MeshTransformMode::WorldIndependent);
    }

    // 화면 좌표에서 정규화된 월드 선택 ray를 만든다.
    PickingRay MakePickingRay(const Math::Camera& camera, float x, float y)
    {
        return { camera.GetPosition(), GizmoMath::RayDirection(camera.GetViewProjMatrix(), camera.GetPosition(), x, y) };
    }

    // 경계 구로 후보를 줄이고 생성 조각은 현재 충돌 hull의 가장 가까운 교점으로 선택한다.
    PickHit PickObject(const Scene& scene, const PickingRay& ray)
    {
        using namespace Math;
        PickHit result;
        for (const auto& object : scene.GetObjects())
        {
            // 바닥처럼 편집 잠금된 객체는 선택에서 제외한다.
            if (!object->IsEditable()) continue;

            // 준비된 메시 경계를 기존 ray와 대조한다.
            for (const auto& mesh : object->GetMeshComponents())
            {
                if (mesh->GetGeometry()->GetSections().empty() || !mesh->HasPoseBounds())
                    continue;
                const BoundingSphere bounds = mesh->GetBoundingSphere(object->GetTransform());
                const Vector3 offset = ray.Origin - bounds.GetCenter();
                const float b = Dot(offset, ray.Direction);
                const float radius = bounds.GetRadius();
                const float c = float(Dot(offset, offset)) - radius * radius;
                const float discriminant = b * b - c;
                if (discriminant < 0.0f)
                    continue;
                const float root = std::sqrt(discriminant);
                const float entry = -b - root;
                const float distance = entry >= 0.0f ? entry : -b + root;

                // 생성 조각의 겹치는 경계 구 대신 실제 계승 hull 표면을 검사한다.
                float nearest = distance;
                if (mesh->GetGeometry()->IsGenerated() && !mesh->GetCollision().ConvexElems.empty())
                {
                    const Matrix4 inverse = Invert(Matrix4(mesh->GetWorldTransform(object->GetTransform())));
                    const Vector3 origin(inverse * ray.Origin);
                    const Vector3 direction = Vector3(inverse * Vector4(ray.Direction, 0));
                    nearest = std::numeric_limits<float>::max();
                    for (const auto& hull : mesh->GetCollision().ConvexElems)
                    {
                        // 로컬 ray를 정규화하지 않아 서로 다른 scale의 조각도 월드 거리로 비교한다.
                        // 충돌 hull의 배정밀도 정점을 선택 계산용 벡터로 변환한다.
                        const auto point = [&hull](uint32_t index)
                        {
                            const auto& v = hull.VertexData[index];
                            return Vector3(float(v.x), float(v.y), float(v.z));
                        };
                        for (size_t i = 0; i + 2 < hull.IndexData.size(); i += 3)
                        {
                            float hit;
                            if (GizmoMath::IntersectTriangle(origin, direction, point(hull.IndexData[i]),
                                point(hull.IndexData[i + 1]), point(hull.IndexData[i + 2]), hit))
                                nearest = std::min(nearest, hit);
                        }
                    }
                }
                result = NearestPickHit(result, {object->GetId(), nearest, mesh->GetId()});
            }
        }
        return result;
    }

    // pick한 컴포넌트가 월드 독립이면 Mesh, 아니면 Owner 대상으로 만든다.
    EditTarget ResolveEditTarget(const Scene& scene, const PickHit& hit)
    {
        // 컴포넌트를 찾아 월드 독립 여부를 읽는다.
        const GameObject* object = scene.FindObject(hit.Id);
        const MeshComponent* mesh = object ? object->FindMeshComponent(hit.MeshId) : nullptr;
        const bool worldIndependent = mesh && mesh->GetTransformMode() == MeshTransformMode::WorldIndependent;

        // 순수 규칙으로 대상을 정한다.
        return EditTarget::FromPick(hit, worldIndependent);
    }

    // 삭제되거나 모드가 바뀐 대상은 무효로 판정한다.
    bool IsEditTargetValid(const Scene& scene, const EditTarget& target)
    {
        // owner가 살아 있어야 한다.
        const GameObject* object = scene.FindObject(target.Object);
        if (!object)
            return false;

        // owner 대상은 owner만 있으면 된다.
        if (target.Kind == EditTargetKind::Owner)
            return true;

        // Mesh 대상은 컴포넌트가 남아 있고 월드 독립이어야 한다.
        const MeshComponent* mesh = object->FindMeshComponent(target.Mesh);
        return mesh && mesh->GetTransformMode() == MeshTransformMode::WorldIndependent;
    }

    // 대상 종류에 맞는 월드 TRS를 구한다.
    bool TryGetEditTargetTransform(const Scene& scene, const EditTarget& target, SceneTransform& out)
    {
        // 무효 대상은 거절한다.
        if (!IsEditTargetValid(scene, target))
            return false;

        // owner 대상은 편집 TRS를 그대로 쓴다.
        const GameObject* object = scene.FindObject(target.Object);
        if (target.Kind == EditTargetKind::Owner)
        {
            out = object->GetTransform();
            return true;
        }

        // Mesh 대상은 컴포넌트 월드 행렬을 TRS로 분해한다.
        const MeshComponent* mesh = object->FindMeshComponent(target.Mesh);
        return SceneTransform::TryFromAffine(mesh->GetWorldTransform(object->GetTransform()), out);
    }

    // 편집 TRS를 바꾸지 않고 생성 조각의 최신 경계 중심을 기즈모 피벗으로 구한다.
    Math::Vector3 GetEditTargetPivot(const Scene& scene, const EditTarget& target, const SceneTransform& frame)
    {
        // 원본과 owner는 기존 원점을 유지하고 생성 Mesh만 조각별 중심을 쓴다.
        const auto* object = scene.FindObject(target.Object);
        const auto* mesh = object && target.IsMesh() ? object->FindMeshComponent(target.Mesh) : nullptr;
        if (mesh && mesh->GetGeometry()->IsGenerated() && mesh->HasPoseBounds())
            return mesh->GetBoundingSphere(object->GetTransform()).GetCenter();
        return frame.GetTranslation();
    }
}
