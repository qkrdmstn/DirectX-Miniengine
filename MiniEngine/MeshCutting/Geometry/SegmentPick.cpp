#include "../pch.h"
#include "SegmentPick.h"

#include <algorithm>
#include <cmath>

namespace MeshCutting
{
    void PickSegments(const SkeletonPose& pose, const CutPlaneFrame& frame, std::vector<SegmentHit>& hits)
    {
        hits.clear();
        const size_t count = pose.jointPositions.size();
        if (pose.parents.size() != count || ValidatePlane(ToSlicePlane(frame)) != SliceStatus::Ok)
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
            if (parent == kNoNode || parent >= count || parent == child)
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
            if (ContainsInPlane(frame, hit.point, 0.0f))
                hits.push_back(hit);
        }
    }

    std::vector<uint32_t> CutJoints(const std::vector<SegmentHit>& hits)
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
