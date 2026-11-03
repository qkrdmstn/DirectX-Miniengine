#pragma once
#include "../../MeshCutting/Geometry/CutPlane.h"
#include <vector>
#include <cstdint>
#include <limits>
#include <cmath>
#include <utility>

namespace Viewer
{
    enum class SegmentJointKind { Segment, Leaf };
    struct SegmentJointSelection
    {
        uint32_t NodeId = UINT32_MAX;
        SegmentJointKind Kind = SegmentJointKind::Leaf;
        uint32_t ChildNodeId = UINT32_MAX;
        Math::Vector3 Intersection{ Math::kZero };
        std::vector<std::vector<uint32_t>> PaletteMasks;
    };

    // 현재 월드 위치와 유효 부모에서 유한 평면을 지나는 segment의 부모 본을 고른다.
    inline SegmentJointSelection SelectSegmentJoint(const std::vector<uint32_t>& parents,
        const std::vector<Math::Vector3>& positions, const std::vector<std::vector<uint32_t>>& palettes,
        const MeshCutting::CutPlaneData& plane, const std::vector<uint8_t>& influencing)
    {
        // 영향 없는 본과 팔레트 중복을 제외하고 가장 가까운 영향 skin 조상을 준비한다.
        using namespace Math;
        SegmentJointSelection result;
        if (positions.size() != parents.size() || influencing.size() != parents.size())
            return result;
        std::vector<bool> skin(parents.size(), false), hasChild(parents.size(), false);
        std::vector<uint32_t> skinParents(parents.size(), UINT32_MAX);
        for (const auto& palette : palettes)
            for (uint32_t node : palette)
            {
                if (node >= parents.size())
                    return result;
                skin[node] = influencing[node] != 0;
            }
        for (uint32_t node = 0; node < parents.size(); ++node)
        {
            if (!skin[node])
                continue;
            uint32_t parent = parents[node];
            size_t depth = 0;
            while (parent != UINT32_MAX && parent < parents.size() && !skin[parent] && depth++ < parents.size())
                parent = parents[parent];
            if (parent == UINT32_MAX)
                continue;
            if (parent >= parents.size() || depth >= parents.size() || parent == node)
                return result;
            skinParents[node] = parent;
            hasChild[parent] = true;
        }

        // 반대 부호 segment의 교점을 사각형 내부에서만 비교한다. 동점은 NodeId 순서를 유지한다.
        const Vector3 center = (plane.WorldCorners[0] + plane.WorldCorners[2]) * 0.5f;
        float best = std::numeric_limits<float>::max();
        for (uint32_t child = 0; child < parents.size(); ++child)
        {
            const uint32_t parent = skinParents[child];
            if (parent == UINT32_MAX)
                continue;
            float a = Dot(Vector4(positions[parent], 1), plane.WorldPlane);
            float b = Dot(Vector4(positions[child], 1), plane.WorldPlane);
            // 월드 거리의 반올림 오차는 평면 위로 맞춰 말단이 segment로 바뀌지 않게 한다.
            if (std::abs(a) <= 1e-6f) a = 0;
            if (std::abs(b) <= 1e-6f) b = 0;
            if (!((a < 0 && b > 0) || (a > 0 && b < 0)))
                continue;
            const Vector3 hit = positions[parent] + (positions[child] - positions[parent]) * (a / (a - b));
            const Vector3 local = Vector3(plane.WorldToPlane * Vector4(hit, 1));
            const float distance = LengthSquare(hit - center);
            if (std::abs(float(local.GetX())) > plane.HalfWidth || std::abs(float(local.GetZ())) > plane.HalfHeight || distance >= best)
                continue;
            best = distance;
            result.NodeId = parent;
            result.ChildNodeId = child;
            result.Kind = SegmentJointKind::Segment;
            result.Intersection = hit;
        }

        // 교차 segment가 없으면 가장 가까운 말단을 고르고 그 위치를 로그 지점으로 쓴다.
        if (result.NodeId == UINT32_MAX)
            for (uint32_t node = 0; node < parents.size(); ++node)
                if (skin[node] && !hasChild[node] && float(LengthSquare(positions[node] - center)) < best)
                {
                    best = LengthSquare(positions[node] - center);
                    result.NodeId = node;
                    result.Intersection = positions[node];
                }

        // section별 팔레트 순서를 보존하여 같은 NodeId의 모든 중복 항목을 표시한다.
        for (const auto& palette : palettes)
        {
            std::vector<uint32_t> mask;
            for (uint32_t node : palette)
                mask.push_back(result.NodeId != UINT32_MAX && node == result.NodeId ? 1u : 0u);
            result.PaletteMasks.push_back(std::move(mask));
        }
        return result;
    }
}
