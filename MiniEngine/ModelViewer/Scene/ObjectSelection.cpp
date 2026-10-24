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

    // 기존 모델 구 선택을 유지하며 공통 월드 거리 결과를 반환한다.
    PickHit PickObject(const Scene& scene, const PickingRay& ray)
    {
        using namespace Math;
        PickHit result;
        for (const auto& object : scene.GetObjects())
        {
            const ModelInstance* model = object->GetModelInstance();
            if (!model)
                continue;
            const BoundingSphere bounds = model->GetBoundingSphere();
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
            result = NearestPickHit(result, {object->GetId(), distance});
        }
        return result;
    }
}
