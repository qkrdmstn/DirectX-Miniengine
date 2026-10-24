#include "CutPlane.h"
#include <algorithm>
#include <cmath>

using namespace Math;
using namespace DirectX;

namespace MeshCutting
{
    namespace
    {
        // SIMD 벡터의 실제 XYZ 성분만 유한한지 검사한다.
        bool IsFinite(Vector3 value)
        {
            return std::isfinite(float(value.GetX())) && std::isfinite(float(value.GetY()))
                && std::isfinite(float(value.GetZ()));
        }

        // 월드 점을 로컬 평면 좌표로 옮긴다.
        Vector3 LocalPoint(const CutPlaneData& data, Vector3 point)
        {
            return Vector3(XMVector3TransformCoord(point, data.WorldToPlane));
        }
    }

    // 유한 양수이며 절반 크기도 표현 가능한 직사각형인지 검사한다.
    bool IsValidCutPlaneDesc(const CutPlaneDesc& desc)
    {
        return std::isfinite(desc.Width) && std::isfinite(desc.Height)
            && desc.Width * 0.5f > 0.0f && desc.Height * 0.5f > 0.0f;
    }

    // 역변환과 역전치 법선을 검증한 뒤 완성된 스냅샷만 공개한다.
    bool TryBuildCutPlaneData(const CutPlaneDesc& desc, const AffineTransform& transform, CutPlaneData& data)
    {
        if (!IsValidCutPlaneDesc(desc) || !IsFinite(transform.GetX()) || !IsFinite(transform.GetY())
            || !IsFinite(transform.GetZ()) || !IsFinite(transform.GetTranslation()))
            return false;
        const XMMATRIX matrix = Matrix4(transform);
        XMVECTOR determinant;
        const XMMATRIX inverse = XMMatrixInverse(&determinant, matrix);
        if (!std::isfinite(XMVectorGetX(determinant)) || XMVectorGetX(determinant) == 0.0f)
            return false;
        for (int i = 0; i < 4; ++i)
            if (XMVector4IsNaN(inverse.r[i]) || XMVector4IsInfinite(inverse.r[i]))
                return false;
        const XMVECTOR rawPlane = XMVector4Transform(XMVectorSet(0, 1, 0, 0), XMMatrixTranspose(inverse));
        if (XMVector4IsNaN(rawPlane) || XMVector4IsInfinite(rawPlane))
            return false;
        const float normalLength = XMVectorGetX(XMVector3Length(rawPlane));
        if (!std::isfinite(normalLength) || normalLength == 0.0f)
            return false;
        CutPlaneData result;
        result.WorldPlane = Vector4(XMVectorScale(rawPlane, 1.0f / normalLength));
        if (XMVector4IsNaN(result.WorldPlane) || XMVector4IsInfinite(result.WorldPlane))
            return false;
        result.WorldToPlane = Matrix4(inverse);
        result.HalfWidth = desc.Width * 0.5f;
        result.HalfHeight = desc.Height * 0.5f;
        const float signs[4][2] = { {-1, -1}, {1, -1}, {1, 1}, {-1, 1} };
        for (int i = 0; i < 4; ++i)
        {
            result.WorldCorners[i] = transform * Vector3(signs[i][0] * result.HalfWidth,
                0, signs[i][1] * result.HalfHeight);
            if (!IsFinite(result.WorldCorners[i]))
                return false;
        }
        data = result;
        return true;
    }

    // 로컬 방향을 재정규화하지 않아 비균일 스케일에서도 월드 t를 보존한다.
    bool IntersectCutPlaneRectangle(const CutPlaneData& data, Vector3 worldRayOrigin,
        Vector3 normalizedWorldRayDirection, float& outT)
    {
        if (!IsFinite(worldRayOrigin) || !IsFinite(normalizedWorldRayDirection))
            return false;
        const Vector3 origin = LocalPoint(data, worldRayOrigin);
        const Vector3 direction(XMVector3TransformNormal(normalizedWorldRayDirection, data.WorldToPlane));
        const float dy = direction.GetY();
        if (dy == 0.0f)
            return false;
        const float t = -float(origin.GetY()) / dy;
        const Vector3 hit = origin + direction * t;
        if (!std::isfinite(t) || t < 0.0f || !IsFinite(hit)
            || std::abs(float(hit.GetX())) > data.HalfWidth
            || std::abs(float(hit.GetZ())) > data.HalfHeight)
            return false;
        outT = t;
        return true;
    }

    // 로컬 XZ 구간 검사로 양 끝이 외부인 통과 선분도 잘라낸다.
    bool ClipCoplanarSegmentToCutPlaneRectangle(const CutPlaneData& data, Vector3 start,
        Vector3 end, float& outT0, float& outT1)
    {
        const Vector3 a = LocalPoint(data, start), b = LocalPoint(data, end);
        if (!IsFinite(a) || !IsFinite(b) || std::abs(float(a.GetY())) > 1e-5f
            || std::abs(float(b.GetY())) > 1e-5f)
            return false;
        float t0 = 0.0f, t1 = 1.0f;
        const float positions[] = { a.GetX(), a.GetZ() };
        const float deltas[] = { float(b.GetX() - a.GetX()), float(b.GetZ() - a.GetZ()) };
        const float halves[] = { data.HalfWidth, data.HalfHeight };
        for (int i = 0; i < 2; ++i)
        {
            if (deltas[i] == 0.0f)
            {
                if (std::abs(positions[i]) > halves[i])
                    return false;
                continue;
            }
            float enter = (-halves[i] - positions[i]) / deltas[i];
            float exit = (halves[i] - positions[i]) / deltas[i];
            if (enter > exit)
                std::swap(enter, exit);
            t0 = (std::max)(t0, enter);
            t1 = (std::min)(t1, exit);
            if (t0 > t1)
                return false;
        }
        outT0 = t0;
        outT1 = t1;
        return true;
    }
}
