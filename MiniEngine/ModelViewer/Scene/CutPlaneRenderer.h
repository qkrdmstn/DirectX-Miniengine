#pragma once
#include "CutPlaneSystem.h"

namespace Viewer
{
    // 평면 스냅샷을 장면 깊이로 가려지는 테두리와 격자로 그린다.
    unsigned RenderCutPlanes(GraphicsContext& context, const CutPlaneSystem& planes,
        ObjectId selected, const Math::Camera& camera, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor);
}
