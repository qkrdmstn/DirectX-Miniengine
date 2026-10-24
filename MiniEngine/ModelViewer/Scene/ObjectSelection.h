#pragma once

#include "Scene.h"

namespace Viewer
{
    struct PickingRay
    {
        Math::Vector3 Origin;
        Math::Vector3 Direction;
    };

    // Coordinates are normalized across the full rendered viewport, with Y down.
    PickingRay MakePickingRay(const Math::Camera& camera, float x, float y);
    ObjectId PickObject(const Scene& scene, const PickingRay& ray);
}
