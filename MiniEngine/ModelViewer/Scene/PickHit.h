#pragma once
#include <cstdint>
#include "ComponentId.h"
#include <limits>

namespace Viewer
{
    using ObjectId = uint64_t;
    constexpr ObjectId InvalidObjectId = 0;
    struct PickHit
    {
        ObjectId Id = InvalidObjectId;
        float WorldT = std::numeric_limits<float>::max();
        ComponentId MeshId = InvalidComponentId;
    };
    // 유효한 앞쪽 후보 중 더 가까운 결과를 선택하며 동률은 기존 결과를 유지한다.
    inline PickHit NearestPickHit(PickHit current, PickHit candidate)
    {
        return candidate.Id != InvalidObjectId && candidate.WorldT >= 0.0f
            && candidate.WorldT < current.WorldT ? candidate : current;
    }

    // 카메라 범위 안의 평면 교점이 reversed-Z 장면 깊이에 가려지는지 검사한다.
    inline bool IsCutPlaneDepthVisible(float planeDepth, float sceneDepth)
    {
        // 투영과 GPU 깊이 계산 사이의 작은 반올림 오차를 허용한다.
        return planeDepth >= 0.0f && planeDepth <= 1.0f && planeDepth + 1e-6f >= sceneDepth;
    }
}
