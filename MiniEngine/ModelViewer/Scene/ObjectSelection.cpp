#include "ObjectSelection.h"
#include "GizmoMath.h"
#include <cmath>
#include <limits>

namespace Viewer
{
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
}
