#pragma once
#include "CutPlaneSystem.h"
#include "ReadbackBuffer.h"

namespace Viewer
{
    // 직전 프레임의 장면 깊이에서 클릭 픽셀 하나를 읽는다.
    float ReadCutPlanePickDepth(unsigned pixelX, unsigned pixelY, ByteAddressBuffer& sample, ReadbackBuffer& readback);

    // 평면 스냅샷을 장면 깊이로 가려지는 테두리와 격자로 그린다.
    unsigned RenderCutPlanes(GraphicsContext& context, const CutPlaneSystem& planes,
        ObjectId selected, const Math::Camera& camera, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor);
}
