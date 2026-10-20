#include "../pch.h"
#include "CutPlane.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace MeshCutting
{
    namespace
    {
        // 정규화된 쿼터니언을 돌려준다. 값이 이상하면 항등 회전이다.
        XMVECTOR NormalizedRotation(const XMFLOAT4& q)
        {
            const XMVECTOR v = XMLoadFloat4(&q);
            const float length = XMVectorGetX(XMVector4Length(v));
            if (!std::isfinite(length) || length < 1e-6f)
                return XMQuaternionIdentity();
            return XMVectorScale(v, 1.0f / length);
        }

        float ClampFloat(float v, float lo, float hi)
        {
            return v < lo ? lo : (v > hi ? hi : v);
        }
    }

    CutPlaneFrame MakeFrame(const CutPlane& plane)
    {
        const XMVECTOR q = NormalizedRotation(plane.rotation);
        CutPlaneFrame frame;
        frame.center = plane.center;
        frame.halfExtents = plane.halfExtents;
        XMStoreFloat3(&frame.axisU, XMVector3Rotate(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), q));
        XMStoreFloat3(&frame.axisV, XMVector3Rotate(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), q));
        XMStoreFloat3(&frame.normal, XMVector3Rotate(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), q));
        return frame;
    }

    SlicePlane ToSlicePlane(const CutPlaneFrame& frame)
    {
        SlicePlane plane;
        plane.point = frame.center;
        plane.normal = frame.normal;
        return plane;
    }

    bool ContainsInPlane(const CutPlaneFrame& frame, const XMFLOAT3& p, float margin)
    {
        const double d[3] = { double(p.x) - frame.center.x, double(p.y) - frame.center.y, double(p.z) - frame.center.z };
        const double u = d[0] * frame.axisU.x + d[1] * frame.axisU.y + d[2] * frame.axisU.z;
        const double v = d[0] * frame.axisV.x + d[1] * frame.axisV.y + d[2] * frame.axisV.z;
        return std::fabs(u) <= double(frame.halfExtents.x) + margin && std::fabs(v) <= double(frame.halfExtents.y) + margin;
    }

    void Translate(CutPlane& plane, const XMFLOAT3& worldDelta)
    {
        plane.center.x += worldDelta.x;
        plane.center.y += worldDelta.y;
        plane.center.z += worldDelta.z;
    }

    void RotateAroundLocalAxis(CutPlane& plane, int axis, float radians)
    {
        const XMVECTOR q = NormalizedRotation(plane.rotation);
        const XMVECTOR localAxis = axis == 0 ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
            : (axis == 1 ? XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f) : XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f));
        // XMQuaternionMultiply(a, b) applies a first, then b: the turn about the local axis comes first.
        const XMVECTOR turned = XMQuaternionMultiply(XMQuaternionRotationAxis(localAxis, radians), q);
        XMStoreFloat4(&plane.rotation, XMQuaternionNormalize(turned));
    }

    void ClampExtents(CutPlane& plane, float minExtent, float maxExtent)
    {
        plane.halfExtents.x = ClampFloat(plane.halfExtents.x, minExtent, maxExtent);
        plane.halfExtents.y = ClampFloat(plane.halfExtents.y, minExtent, maxExtent);
    }

    void ScaleUniform(CutPlane& plane, float factor, float minExtent, float maxExtent)
    {
        if (!(factor > 0.0f) || !std::isfinite(factor))
            return;
        const float x = plane.halfExtents.x, y = plane.halfExtents.y;
        // The largest range of factors that keeps both sides inside [minExtent, maxExtent].
        const float lo = std::max(minExtent / x, minExtent / y);
        const float hi = std::min(maxExtent / x, maxExtent / y);
        if (lo > hi)
        {
            ClampExtents(plane, minExtent, maxExtent);
            return;
        }
        factor = ClampFloat(factor, lo, hi);
        plane.halfExtents.x = x * factor;
        plane.halfExtents.y = y * factor;
    }

    void ScaleU(CutPlane& plane, float factor, float minExtent, float maxExtent)
    {
        if (!(factor > 0.0f) || !std::isfinite(factor))
            return;
        plane.halfExtents.x = ClampFloat(plane.halfExtents.x * factor, minExtent, maxExtent);
    }

    bool LoopsInsidePlane(const CutPlaneFrame& frame, const PatchPiece& positive,
        const std::vector<std::vector<uint32_t>>& loops, float margin, std::string* detail)
    {
        for (size_t l = 0; l < loops.size(); ++l)
        {
            for (size_t i = 0; i < loops[l].size(); ++i)
            {
                const uint32_t node = loops[l][i];
                if (node >= positive.nodes.size() || !ContainsInPlane(frame, positive.nodes[node].position, margin))
                {
                    if (detail)
                        *detail = "loop " + std::to_string(l) + " point " + std::to_string(i) + " (node "
                            + std::to_string(node) + ") is outside the cut plane rectangle";
                    return false;
                }
            }
        }
        return true;
    }
}
