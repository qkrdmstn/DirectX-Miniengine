#pragma once

// 절단 평면: 어떤 joint에도 묶이지 않는 world 공간의 유한 사각형.
// 설계: Docs/PlaneFirstCutDesign.md 2절. Model, Core에 의존하지 않는다.

#include "PatchGeometry.h"
#include <string>
#include <vector>

namespace MeshCutting
{
    // 로컬 +Z가 법선, +X/+Y가 평면 축이다.
    struct CutPlane
    {
        DirectX::XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f };   // 쿼터니언 (x, y, z, w)
        DirectX::XMFLOAT2 halfExtents = { 1.0f, 1.0f };             // 로컬 X/Y 방향 반 길이
    };

    // CutPlane에서 매번 계산한다. 축은 정규 직교이고 u x v = n이다.
    struct CutPlaneFrame
    {
        DirectX::XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 axisU = { 1.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 axisV = { 0.0f, 1.0f, 0.0f };
        DirectX::XMFLOAT3 normal = { 0.0f, 0.0f, 1.0f };
        DirectX::XMFLOAT2 halfExtents = { 1.0f, 1.0f };
    };

    // rotation이 0 길이이거나 유한하지 않으면 항등 회전으로 본다.
    CutPlaneFrame MakeFrame(const CutPlane& plane);

    // 슬라이서에는 무한 평면으로 넘긴다. 사각형의 크기는 슬라이스 뒤 루프 검사에만 쓴다.
    SlicePlane ToSlicePlane(const CutPlaneFrame& frame);

    // p를 평면에 내린 점이 사각형 안(가장자리 + margin 이내)에 있는가. 법선 방향 거리는 보지 않는다.
    bool ContainsInPlane(const CutPlaneFrame& frame, const DirectX::XMFLOAT3& p, float margin);

    // ---- 조작. 매번 쿼터니언을 정규화하고, halfExtents는 [minExtent, maxExtent]로 제한한다. ----
    void Translate(CutPlane& plane, const DirectX::XMFLOAT3& worldDelta);

    // axis: 0 = 평면 U축, 1 = V축, 2 = 법선축. 평면 자체의 축을 기준으로 돌린다(누적해도 축이 어긋나지 않는다).
    void RotateAroundLocalAxis(CutPlane& plane, int axis, float radians);

    // 두 halfExtents에 같은 비율을 곱한다. 한쪽이 범위를 벗어나면 비율이 유지되도록 factor를 줄인다.
    void ScaleUniform(CutPlane& plane, float factor, float minExtent, float maxExtent);

    // U축(halfExtents.x)만 바꾼다.
    void ScaleU(CutPlane& plane, float factor, float minExtent, float maxExtent);

    // halfExtents를 범위 안으로 넣는다.
    void ClampExtents(CutPlane& plane, float minExtent, float maxExtent);

    // 슬라이스 결과의 모든 루프 점이 사각형 안(margin 포함)인지 검사한다. loops는 positive 조각의 노드
    // 번호다. 하나라도 밖이면 false이고 detail에 어느 루프의 몇 번째 점인지 적는다.
    bool LoopsInsidePlane(const CutPlaneFrame& frame, const PatchPiece& positive,
        const std::vector<std::vector<uint32_t>>& loops, float margin, std::string* detail = nullptr);
}
