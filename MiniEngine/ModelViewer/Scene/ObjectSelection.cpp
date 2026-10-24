#include "ObjectSelection.h"
#include "GizmoMath.h"
#include <cmath>
#include <limits>

namespace Viewer
{
    PickingRay MakePickingRay(const Math::Camera& camera, float x, float y)
    {
        return { camera.GetPosition(), GizmoMath::RayDirection(camera.GetViewProjMatrix(), camera.GetPosition(), x, y) };
    }

    ObjectId PickObject(const Scene& scene, const PickingRay& ray)
    {
        using namespace Math;
        float nearest = std::numeric_limits<float>::max();
        ObjectId result = InvalidObjectId;
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
            if (distance >= 0.0f && distance < nearest)
            {
                nearest = distance;
                result = object->GetId();
            }
        }
        return result;
    }
}
