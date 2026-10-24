// VS Developer PowerShell, repository root; substitute an external output directory:
// cl /nologo /utf-8 /std:c++17 /EHsc /W4 /I MiniEngine/Core /Fo:<external>/ /Fe:<external>/CutPlaneTests.exe
//    MiniEngine/MeshCutting/tests/CutPlaneTests.cpp MiniEngine/MeshCutting/Geometry/CutPlane.cpp
#include "../Geometry/CutPlane.h"
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace Math;
using namespace MeshCutting;

// 실패한 계약을 독립 실행 테스트의 오류로 보고한다.
void Check(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

// 월드 거리와 선분 매개변수의 오차를 검사한다.
bool Near(float a, float b)
{
    return std::abs(a - b) < 1e-4f;
}

// 크기·변환·ray·선분 계약을 GPU 없이 검증한다.
int main()
{
    try
    {
        CutPlaneData data;
        Check(!IsValidCutPlaneDesc({0, 2}) && !IsValidCutPlaneDesc({-1, 2})
            && !IsValidCutPlaneDesc({2, std::numeric_limits<float>::infinity()}), "invalid sizes");
        Check(TryBuildCutPlaneData({2, 4}, AffineTransform(kIdentity), data), "identity build");
        float t = -1, t0 = -1, t1 = -1;
        Check(IntersectCutPlaneRectangle(data, Vector3(1, 3, 2), Vector3(0, -1, 0), t)
            && Near(t, 3), "inclusive corner");
        Check(!IntersectCutPlaneRectangle(data, Vector3(2, 3, 0), Vector3(0, -1, 0), t), "outside");
        Check(!IntersectCutPlaneRectangle(data, Vector3(0, 3, 0), Vector3(0, 1, 0), t), "behind");
        Check(!IntersectCutPlaneRectangle(data, Vector3(0, 0, 0), Vector3(1, 0, 0), t), "coplanar ray");
        Check(!IntersectCutPlaneRectangle(data, Vector3(0, 3, 0), Vector3(1, 0, 0), t), "parallel ray");
        Check(ClipCoplanarSegmentToCutPlaneRectangle(data, Vector3(-3, 0, 0), Vector3(3, 0, 0), t0, t1)
            && Near(t0, 1.0f / 3) && Near(t1, 2.0f / 3), "outside crossing segment");
        Check(ClipCoplanarSegmentToCutPlaneRectangle(data, Vector3(1, 0, 2), Vector3(1, 0, 2), t0, t1), "boundary point");
        Check(!ClipCoplanarSegmentToCutPlaneRectangle(data, Vector3(-3, 1, 0), Vector3(3, 1, 0), t0, t1), "noncoplanar");
        Check(!ClipCoplanarSegmentToCutPlaneRectangle(data, Vector3(-3, 0, 3), Vector3(3, 0, 3), t0, t1), "outside segment");
        const AffineTransform transform(Vector3(-2, 0, 0), Vector3(1, 3, 0), Vector3(0, 0, 4), Vector3(5, 6, 7));
        Check(TryBuildCutPlaneData({2, 4}, transform, data), "reflection shear scale build");
        Check(Near(DirectX::XMVectorGetX(DirectX::XMVector3Length(data.WorldPlane)), 1)
            && Near(float(data.WorldPlane.GetW()), -6), "normalized translated plane");
        Check(IntersectCutPlaneRectangle(data, Vector3(5, 12, 7), Vector3(0, -1, 0), t)
            && Near(t, 6), "world ray distance");
        for (const Vector3 corner : data.WorldCorners)
        {
            const float distance = DirectX::XMVectorGetX(DirectX::XMVector4Dot(
                DirectX::XMVectorSetW(corner, 1), data.WorldPlane));
            Check(Near(distance, 0), "world plane corners");
        }
        Check(!TryBuildCutPlaneData({2, 4}, AffineTransform::MakeScale(Vector3(1, 0, 1)), data), "singular build");
        Check(Near(float(data.HalfWidth), 1), "failed build preserves output");
        const AffineTransform rotated = AffineTransform::MakeXRotation(DirectX::XM_PIDIV2);
        Check(TryBuildCutPlaneData({2, 4}, rotated, data), "rotated build");
        Check(IntersectCutPlaneRectangle(data, rotated * Vector3(0, 3, 0),
            Vector3(0, 0, -1), t) && Near(t, 3), "rotated ray");
        Check(TryBuildCutPlaneData({2, 4}, AffineTransform::MakeScale(Vector3(1, -2, 1)), data)
            && Near(float(data.WorldPlane.GetY()), -1), "reflected normal orientation");
        const float diagonal = 1.0f / std::sqrt(3.0f);
        const AffineTransform huge(Vector3(1, -1, 0), Vector3(diagonal, diagonal, diagonal),
            Vector3(1, 1, -2), Vector3(2e38f, 2e38f, 2e38f));
        Check(!TryBuildCutPlaneData({2, 4}, huge, data), "overflow plane rejected");
        std::puts("CutPlaneTests passed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::printf("CutPlaneTests failed: %s\n", error.what());
        return 1;
    }
}
