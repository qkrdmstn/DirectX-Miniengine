#pragma once
#include <cstdint>
#include <limits>

namespace Viewer
{
    using ObjectId = uint64_t;
    constexpr ObjectId InvalidObjectId = 0;
    struct PickHit
    {
        ObjectId Id = InvalidObjectId;
        float WorldT = std::numeric_limits<float>::max();
    };
    // 유효한 앞쪽 후보 중 더 가까운 결과를 선택하며 동률은 기존 결과를 유지한다.
    inline PickHit NearestPickHit(PickHit current, PickHit candidate)
    {
        return candidate.Id != InvalidObjectId && candidate.WorldT >= 0.0f
            && candidate.WorldT < current.WorldT ? candidate : current;
    }
}
