#include "ObjectSelection.h"
#include "GizmoMath.h"
#include "PhysicsSystem.h"
#include <cmath>
#include <limits>

namespace Viewer
{
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

    // 모든 메시 컴포넌트를 월드 구로 검사하고 owner와 component ID를 함께 반환한다.
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
                result = NearestPickHit(result, {object->GetId(), distance, mesh->GetId()});
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
}
