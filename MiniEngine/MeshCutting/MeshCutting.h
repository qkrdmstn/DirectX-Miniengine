#pragma once

#include <vector>
#include "VectorMath.h"

namespace MeshCutting
{
    struct InputStroke
    {
        // 화면 픽셀 좌표를 입력 순서대로 저장.
        // x, y를 사용하고 z는 0으로 둔다.
        std::vector<Math::Vector3> points;
    };

    void ReceiveStroke(const InputStroke& stroke);
}