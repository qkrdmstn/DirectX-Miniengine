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
    // 모델 구의 교점을 월드 거리와 함께 반환한다.
    PickHit PickObject(const Scene& scene, const PickingRay& ray);
}
