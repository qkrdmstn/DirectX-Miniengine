// 이 파일은 절단 평면의 계산과 조작(CutPlane)과, 평면이 지나는 뼈 선분 찾기(SegmentPick)를 구현한다.
// 기즈모 계산은 CutPlaneGizmo.cpp에 있다.
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

        // p를 평면에 내린 점이 사각형 안(가장자리 + margin 이내)에 있는가. 법선 방향 거리는 보지 않는다.
        bool FrameContains(const CutPlane::Frame& frame, const XMFLOAT3& p, float margin)
        {
            const double d[3] = { double(p.x) - frame.center.x, double(p.y) - frame.center.y, double(p.z) - frame.center.z };
            const double u = d[0] * frame.axisU.x + d[1] * frame.axisU.y + d[2] * frame.axisU.z;
            const double v = d[0] * frame.axisV.x + d[1] * frame.axisV.y + d[2] * frame.axisV.z;
            return std::fabs(u) <= double(frame.halfExtents.x) + margin && std::fabs(v) <= double(frame.halfExtents.y) + margin;
        }
    }

    CutPlane::CutPlane(const XMFLOAT3& center, const XMFLOAT4& rotation, const XMFLOAT2& halfExtents)
    {
        m_Shape.center = center;
        m_Shape.rotation = rotation;
        m_Shape.halfExtents = halfExtents;
    }

    // 로컬 +Z가 point에서 normal 방향이 되는 회전을 쓴다. 크기는 1e6이라 모델이 어디에 있어도 사각형 검사를 통과한다.
    // normal이 0 길이이거나 유한하지 않으면 항등 회전이다.
    CutPlane CutPlane::MakeInfinite(const XMFLOAT3& point, const XMFLOAT3& normal)
    {
        const double length = std::sqrt(double(normal.x) * normal.x + double(normal.y) * normal.y + double(normal.z) * normal.z);
        XMFLOAT4 rotation = { 0.0f, 0.0f, 0.0f, 1.0f };
        if (std::isfinite(length) && length > 1e-6)
        {
            const double n[3] = { normal.x / length, normal.y / length, normal.z / length };
            if (n[2] < -1.0 + 1e-9)
                rotation = { 1.0f, 0.0f, 0.0f, 0.0f };     // 반대 방향: X축으로 180도
            else
            {
                // +Z에서 n으로 가는 최단 회전: 축 = z x n, w = 1 + z . n
                const XMVECTOR q = XMVectorSet(float(-n[1]), float(n[0]), 0.0f, float(1.0 + n[2]));
                XMStoreFloat4(&rotation, XMQuaternionNormalize(q));
            }
        }
        const float huge = 1.0e6f;
        return CutPlane(point, rotation, { huge, huge });
    }

    // 모델 bounds 중심에 놓는 첫 평면: 법선은 world +Y(수평)이고 반 길이는 반지름의 절반이다.
    // 로컬 +Z가 world +Y가 되도록 X축으로 -90도 돌린다 (Z -> -Y를 뒤집는다).
    CutPlane CutPlane::MakeHorizontal(const XMFLOAT3& center, float modelRadius)
    {
        CutPlane plane;
        plane.m_Shape.center = center;
        XMStoreFloat4(&plane.m_Shape.rotation, XMQuaternionRotationAxis(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), -XM_PIDIV2));
        plane.m_Shape.halfExtents = { modelRadius * 0.5f, modelRadius * 0.5f };
        plane.m_MinExtent = modelRadius * 0.01f;
        plane.m_MaxExtent = modelRadius * 3.0f;
        plane.ClampExtents();
        return plane;
    }

    // rotation이 0 길이이거나 유한하지 않으면 항등 회전으로 본다.
    CutPlane::Frame CutPlane::GetFrame() const
    {
        const XMVECTOR q = NormalizedRotation(m_Shape.rotation);
        Frame frame;
        frame.center = m_Shape.center;
        frame.halfExtents = m_Shape.halfExtents;
        XMStoreFloat3(&frame.axisU, XMVector3Rotate(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), q));
        XMStoreFloat3(&frame.axisV, XMVector3Rotate(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), q));
        XMStoreFloat3(&frame.normal, XMVector3Rotate(XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f), q));
        return frame;
    }

    // 사각형의 크기는 슬라이스 뒤 루프 검사(ContainsLoops)에만 쓴다.
    SlicePlane CutPlane::ToSlicePlane() const
    {
        const Frame frame = GetFrame();
        SlicePlane plane;
        plane.point = frame.center;
        plane.normal = frame.normal;
        return plane;
    }

    bool CutPlane::Contains(const XMFLOAT3& p, float margin) const
    {
        return FrameContains(GetFrame(), p, margin);
    }

    // 슬라이스 결과의 모든 루프 점이 사각형 안(margin 포함)인지 검사한다. loops는 positive 조각의 노드 번호다.
    // 하나라도 밖이면 false이고 error에 어느 루프의 몇 번째 점인지 적는다.
    bool CutPlane::ContainsLoops(const MeshGeometry& positive, const std::vector<std::vector<uint32_t>>& loops,
        float margin, std::string* error) const
    {
        const Frame frame = GetFrame();
        for (size_t l = 0; l < loops.size(); ++l)
        {
            for (size_t i = 0; i < loops[l].size(); ++i)
            {
                const uint32_t vertex = loops[l][i];
                if (vertex >= positive.vertices.size() || !FrameContains(frame, positive.vertices[vertex].position, margin))
                {
                    if (error)
                        *error = "loop " + std::to_string(l) + " point " + std::to_string(i) + " (vertex "
                            + std::to_string(vertex) + ") is outside the cut plane rectangle";
                    return false;
                }
            }
        }
        return true;
    }

    // ---- 조작. 매번 쿼터니언을 정규화하고, halfExtents는 [m_MinExtent, m_MaxExtent]로 제한한다. ----

    void CutPlane::SetExtentLimits(float minExtent, float maxExtent)
    {
        m_MinExtent = minExtent;
        m_MaxExtent = maxExtent;
    }

    void CutPlane::Translate(const XMFLOAT3& worldDelta)
    {
        m_Shape.center.x += worldDelta.x;
        m_Shape.center.y += worldDelta.y;
        m_Shape.center.z += worldDelta.z;
    }

    // 평면 자체의 축을 기준으로 돌린다 (누적해도 축이 어긋나지 않는다).
    void CutPlane::RotateAroundLocalAxis(int axis, float radians)
    {
        const XMVECTOR q = NormalizedRotation(m_Shape.rotation);
        const XMVECTOR localAxis = axis == 0 ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
            : (axis == 1 ? XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f) : XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f));
        // XMQuaternionMultiply(a, b) applies a first, then b: the turn about the local axis comes first.
        const XMVECTOR turned = XMQuaternionMultiply(XMQuaternionRotationAxis(localAxis, radians), q);
        XMStoreFloat4(&m_Shape.rotation, XMQuaternionNormalize(turned));
    }

    void CutPlane::ClampExtents()
    {
        m_Shape.halfExtents.x = Clamp(m_Shape.halfExtents.x, m_MinExtent, m_MaxExtent);
        m_Shape.halfExtents.y = Clamp(m_Shape.halfExtents.y, m_MinExtent, m_MaxExtent);
    }

    // 두 halfExtents에 같은 비율을 곱한다. 한쪽이 범위를 벗어나면 비율이 유지되도록 factor를 줄인다.
    void CutPlane::ScaleUniform(float factor)
    {
        if (!(factor > 0.0f) || !std::isfinite(factor))
            return;
        const float x = m_Shape.halfExtents.x, y = m_Shape.halfExtents.y;
        // The largest range of factors that keeps both sides inside [m_MinExtent, m_MaxExtent].
        const float lo = std::max(m_MinExtent / x, m_MinExtent / y);
        const float hi = std::min(m_MaxExtent / x, m_MaxExtent / y);
        if (lo > hi)
        {
            ClampExtents();
            return;
        }
        factor = Clamp(factor, lo, hi);
        m_Shape.halfExtents.x = x * factor;
        m_Shape.halfExtents.y = y * factor;
    }

    // U축(halfExtents.x)만 바꾼다.
    void CutPlane::ScaleU(float factor)
    {
        if (!(factor > 0.0f) || !std::isfinite(factor))
            return;
        m_Shape.halfExtents.x = Clamp(m_Shape.halfExtents.x * factor, m_MinExtent, m_MaxExtent);
    }

    // 선분마다 양 끝이 평면의 어느 쪽인지 본다. 부호 거리가 0 이상이면 + 쪽이므로, 평면이 joint를 정확히
    // 지나도 사슬의 선분 하나에만 걸린다. 교차점이 사각형 안(Contains, margin 0)인 선분만 돌려주고,
    // 순서는 childJoint 오름차순이다. 평면이 올바르지 않거나 jointPositions와 parents 크기가 다르면 hits는 비어 있다.
    void CutPlane::PickSegments(const SkeletonPose& pose, std::vector<SegmentHit>& hits) const
    {
        hits.clear();
        const size_t count = pose.jointPositions.size();
        const Frame frame = GetFrame();
        if (pose.parents.size() != count || ValidatePlane(ToSlicePlane()) != SliceStatus::Ok)
            return;

        const double n[3] = { frame.normal.x, frame.normal.y, frame.normal.z };
        const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        const auto signedDistance = [&](const DirectX::XMFLOAT3& p)
        {
            return ((double(p.x) - frame.center.x) * n[0] + (double(p.y) - frame.center.y) * n[1]
                + (double(p.z) - frame.center.z) * n[2]) / length;
        };

        for (uint32_t child = 0; child < count; ++child)
        {
            const uint32_t parent = pose.parents[child];
            if (parent == kInvalidIndex || parent >= count || parent == child)
                continue;
            const DirectX::XMFLOAT3& a = pose.jointPositions[parent];
            const DirectX::XMFLOAT3& b = pose.jointPositions[child];
            const double da = signedDistance(a), db = signedDistance(b);
            if ((da >= 0.0) == (db >= 0.0))
                continue;

            // The signs differ, so da - db is not zero.
            const double t = da / (da - db);
            SegmentHit hit;
            hit.parentJoint = parent;
            hit.childJoint = child;
            hit.t = float(t);
            hit.point = { float(a.x + (double(b.x) - a.x) * t), float(a.y + (double(b.y) - a.y) * t),
                float(a.z + (double(b.z) - a.z) * t) };
            if (FrameContains(frame, hit.point, 0.0f))
                hits.push_back(hit);
        }
    }

    // 절단 joint 집합: hits의 parentJoint를 중복 없이 오름차순으로.
    std::vector<uint32_t> CutPlane::CutJoints(const std::vector<SegmentHit>& hits)
    {
        std::vector<uint32_t> joints;
        joints.reserve(hits.size());
        for (const SegmentHit& hit : hits)
            joints.push_back(hit.parentJoint);
        std::sort(joints.begin(), joints.end());
        joints.erase(std::unique(joints.begin(), joints.end()), joints.end());
        return joints;
    }
}
