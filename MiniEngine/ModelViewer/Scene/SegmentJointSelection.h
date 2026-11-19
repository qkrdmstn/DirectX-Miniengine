#pragma once
#include "../../MeshCutting/Geometry/CutPlane.h"
#include "../../Model/LatticeTopology.h"
#include <vector>
#include <cstdint>
#include <limits>
#include <cmath>
#include <utility>
#include <map>
#include <tuple>

namespace Viewer
{
    struct RigCutSegment
    {
        uint32_t OwnerId, OwnerIndex, ParentNode, ChildNode;
        Math::Vector3 Parent, Child; // 월드 끝점이며 입력은 owner 및 Asset.Joints 순서다.
        bool Live;
    };
    struct RigCutJoint
    {
        uint32_t ParentNode, ChildNode;
        Math::Vector3 Intersection;
        uint32_t ChildSide; // IntersectLatticeSegment와 동일: 양의 반공간=0, 음의 반공간=1.
    };
    enum class RigCutResult { NoCrossing, AlreadyBroken, Selected, ChildSideMismatch };
    struct RigCutSelection
    {
        RigCutResult Result = RigCutResult::NoCrossing;
        uint32_t OwnerId = UINT32_MAX;
        uint32_t OwnerIndex = UINT32_MAX;
        std::vector<RigCutJoint> Joints;
    };

    // 유한 사각형의 live 교차에서 owner와 최상위 P 집합 및 교차 snapshot을 고른다.
    inline RigCutSelection SelectRigCutJoints(const std::vector<RigCutSegment>& segments,
        const std::vector<std::vector<uint32_t>>& ownerParents, const MeshCutting::CutPlaneData& plane)
    {
        // 엄격한 교차와 사각형 검사를 통과한 선분만 입력 순서대로 수집한다.
        using namespace Math;
        RigCutSelection result;
        std::vector<std::pair<size_t, RigCutJoint>> crossings;
        const Vector3 center = (plane.WorldCorners[0] + plane.WorldCorners[2]) * 0.5f;
        auto best = std::make_tuple(std::numeric_limits<float>::max(), UINT32_MAX, UINT32_MAX);
        for (size_t i = 0; i < segments.size(); ++i)
        {
            const auto& segment = segments[i];
            const float a = Dot(plane.WorldPlane, Vector4(segment.Parent, 1));
            const float b = Dot(plane.WorldPlane, Vector4(segment.Child, 1));
            if (!((a < 0 && b > 0) || (a > 0 && b < 0)))
                continue;
            const Vector3 hit = segment.Parent + (segment.Child - segment.Parent) * (a / (a - b));
            const Vector3 local(plane.WorldToPlane * Vector4(hit, 1));
            if (!(std::abs(float(local.GetX())) <= plane.HalfWidth
                && std::abs(float(local.GetZ())) <= plane.HalfHeight))
                continue;
            result.Result = RigCutResult::AlreadyBroken;
            if (!segment.Live)
                continue;
            crossings.push_back({i, {segment.ParentNode, segment.ChildNode, hit, b > a ? 0u : 1u}});
            const auto key = std::make_tuple(float(LengthSquare(hit - center)), segment.OwnerId, segment.ParentNode);
            if (key >= best)
                continue;
            best = key;
            result.OwnerId = segment.OwnerId;
            result.OwnerIndex = segment.OwnerIndex;
        }
        if (crossings.empty())
            return result;

        // 선택 owner의 같은 P는 최단 교차만 남기고 동점은 먼저 입력된 자식을 보존한다.
        std::map<uint32_t, RigCutJoint> candidates;
        for (const auto& crossing : crossings)
        {
            if (segments[crossing.first].OwnerIndex != result.OwnerIndex)
                continue;
            const auto& joint = crossing.second;
            auto found = candidates.find(joint.ParentNode);
            if (found == candidates.end())
                candidates.emplace(joint.ParentNode, joint);
            else if (float(LengthSquare(joint.Intersection - center))
                < float(LengthSquare(found->second.Intersection - center)))
                found->second = joint;
        }

        // 부모 사슬의 탐색 횟수를 제한하고 조상 P가 있는 자손을 제거해 P 오름차순으로 낸다.
        const auto& parents = ownerParents.at(result.OwnerIndex);
        result.Result = RigCutResult::Selected;
        for (const auto& entry : candidates)
        {
            uint32_t ancestor = entry.first < parents.size() ? parents[entry.first] : UINT32_MAX;
            bool covered = false;
            for (size_t depth = 0; ancestor < parents.size() && depth < parents.size(); ++depth)
            {
                covered = covered || (ancestor != entry.first && candidates.count(ancestor) != 0);
                ancestor = parents[ancestor];
            }
            if (covered)
                continue;
            result.Joints.push_back(entry.second);
            if (entry.second.ChildSide != result.Joints.front().ChildSide)
                result.Result = RigCutResult::ChildSideMismatch;
        }
        return result;
    }

    enum class SegmentJointKind { Segment, Leaf };
    struct SegmentJointSelection
    {
        uint32_t NodeId = UINT32_MAX;
        SegmentJointKind Kind = SegmentJointKind::Leaf;
        uint32_t ChildNodeId = UINT32_MAX;
        Math::Vector3 Intersection{ Math::kZero };
        std::vector<std::vector<uint32_t>> PaletteMasks;
    };

    // 보존한 선택 자식을 우선하고 snapshot이 없을 때만 직접 자식을 허용한다.
    inline bool IsSelectedSegmentChild(uint32_t node, uint32_t child, uint32_t parent, uint32_t snapshotChild)
    {
        // 선택 선분은 중간 비영향 또는 비팔레트 조상을 건너뛸 수 있다.
        return snapshotChild != UINT32_MAX ? child == snapshotChild : parent == node;
    }

    // 두 선택기가 같은 영향 팔레트 조상을 쓰도록 유효 선분 계층을 만든다.
    inline bool BuildSegmentJointParents(const std::vector<uint32_t>& parents,
        const std::vector<std::vector<uint32_t>>& palettes, const std::vector<uint8_t>& influencing,
        std::vector<uint32_t>& skinParents, std::vector<bool>& hasChild)
    {
        // 영향 없는 본과 팔레트 중복을 제외하고 가장 가까운 영향 skin 조상을 준비한다.
        if (influencing.size() != parents.size())
            return false;
        std::vector<bool> skin(parents.size(), false);
        hasChild.assign(parents.size(), false);
        skinParents.assign(parents.size(), UINT32_MAX);
        for (const auto& palette : palettes)
            for (uint32_t node : palette)
            {
                if (node >= parents.size())
                    return false;
                skin[node] = influencing[node] != 0;
            }

        // 각 영향 본에서 가장 가까운 영향 팔레트 조상까지 중간 노드를 건너뛴다.
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
                return false;
            skinParents[node] = parent;
            hasChild[parent] = true;
        }
        return true;
    }

    // P 집합의 각 본 자체만 section별 팔레트 선택 마스크로 합친다.
    inline std::vector<std::vector<uint32_t>> BuildJointUnionMasks(const std::vector<uint32_t>& parents,
        const std::vector<std::vector<uint32_t>>& palettes, const std::vector<uint32_t>& roots)
    {
        // 중복 P와 팔레트 항목은 같은 비트로 모으고 잘못된 P는 거절한다.
        std::vector<bool> selected(parents.size(), false);
        for (uint32_t root : roots)
        {
            if (root >= parents.size())
                return {};
            selected[root] = true;
        }

        // 팔레트 본 자체의 선택 비트를 기록하고 계층 유효성과 순환을 검사한다.
        std::vector<std::vector<uint32_t>> masks;
        for (const auto& palette : palettes)
        {
            std::vector<uint32_t> mask;
            for (uint32_t node : palette)
            {
                const bool included = node < selected.size() && selected[node];
                size_t depth = 0;
                while (node != UINT32_MAX)
                {
                    if (node >= parents.size() || depth++ >= parents.size())
                        return {};
                    node = parents[node];
                }
                mask.push_back(included ? 1u : 0u);
            }
            masks.push_back(std::move(mask));
        }
        return masks;
    }

    struct CutBodyGroups
    {
        std::vector<uint32_t> NodeGroups; // 0=A, i=B_i.
        std::vector<uint32_t> AttachNodes;
        std::vector<std::vector<uint8_t>> FrozenPalettes;
    };

    // 서로 독립인 P 집합에서 몸체 그룹과 부착 노드 및 고정 팔레트를 만든다.
    inline CutBodyGroups BuildCutBodyGroups(const std::vector<uint32_t>& parents,
        const std::vector<uint32_t>& joints, const std::vector<uint32_t>& roots)
    {
        // 각 P의 입력 순서를 몸체 번호로 보존하고 중복 루트를 거절한다.
        CutBodyGroups result;
        if (roots.empty())
            return result;
        std::vector<uint32_t> rootGroups(parents.size(), 0);
        for (size_t i = 0; i < roots.size(); ++i)
        {
            if (roots[i] >= parents.size() || rootGroups[roots[i]])
                return {};
            rootGroups[roots[i]] = uint32_t(i + 1);
        }

        // 전체 계층에서 노드 소속을 구하고 순환과 중첩 P를 거절한다.
        result.NodeGroups.resize(parents.size(), 0);
        for (uint32_t node = 0; node < parents.size(); ++node)
        {
            size_t depth = 0;
            for (uint32_t ancestor = node; ancestor != UINT32_MAX; ancestor = parents[ancestor])
            {
                if (ancestor >= parents.size() || depth++ >= parents.size())
                    return {};
                if (rootGroups[ancestor])
                {
                    if (result.NodeGroups[node])
                        return {};
                    result.NodeGroups[node] = rootGroups[ancestor];
                }
            }
        }

        // A는 모든 P 부모의 최소 공통 조상에 붙이고 B_i는 P_i에 붙인다.
        uint32_t common = parents[roots.front()];
        for (size_t i = 1; i < roots.size(); ++i)
        {
            while (common != UINT32_MAX)
            {
                uint32_t ancestor = parents[roots[i]];
                while (ancestor != UINT32_MAX && ancestor != common)
                    ancestor = parents[ancestor];
                if (ancestor == common)
                    break;
                common = parents[common];
            }
        }
        result.AttachNodes.push_back(common);
        result.AttachNodes.insert(result.AttachNodes.end(), roots.begin(), roots.end());

        // 팔레트 순서와 중복을 보존하며 자기 그룹 밖의 본만 고정한다.
        result.FrozenPalettes.resize(result.AttachNodes.size());
        for (size_t body = 0; body < result.AttachNodes.size(); ++body)
        {
            for (uint32_t joint : joints)
            {
                if (joint >= parents.size())
                    return {};
                result.FrozenPalettes[body].push_back(result.NodeGroups[joint] != body);
            }
        }
        return result;
    }

    struct CrossingJoint
    {
        uint32_t NodeId = UINT32_MAX; // 끊는 P_i이며 B_i의 루트다.
        uint32_t ChildNodeId = UINT32_MAX; // 중간 비영향 노드를 건너뛴 선분 snapshot이다.
        Math::Vector3 Intersection{ Math::kZero };
        uint32_t ChildSide = 0; // 부모에서 자식으로 향하는 B_i 반공간이다.
    };

    // 무한 평면 교차 P_i 중 조상이 선택되지 않은 최상위 집합을 NodeId 순서로 반환한다.
    inline std::vector<CrossingJoint> SelectCrossingJoints(const std::vector<uint32_t>& parents,
        const std::vector<Math::Vector3>& positions, const std::vector<std::vector<uint32_t>>& palettes,
        const MeshCutting::CutPlaneData& plane, const std::vector<uint8_t>& influencing)
    {
        // 유한 선택기와 같은 유효 조상을 사용하며 잘못된 입력은 빈 집합으로 거절한다.
        using namespace Math;
        std::vector<uint32_t> skinParents;
        std::vector<bool> hasChild;
        if (positions.size() != parents.size()
            || !BuildSegmentJointParents(parents, palettes, influencing, skinParents, hasChild))
            return {};
        std::vector<CrossingJoint> candidates(parents.size());
        const Vector3 center = (plane.WorldCorners[0] + plane.WorldCorners[2]) * 0.5f;

        // 같은 P의 교차는 중심에 가장 가까운 자식을 보존하고 동점은 작은 자식 번호로 정한다.
        for (uint32_t child = 0; child < parents.size(); ++child)
        {
            const uint32_t parent = skinParents[child];
            if (parent == UINT32_MAX)
                continue;
            float fraction = 0;
            uint32_t side = 0;
            if (!IntersectLatticeSegment(Dot(Vector4(positions[parent], 1), plane.WorldPlane),
                Dot(Vector4(positions[child], 1), plane.WorldPlane), fraction, side))
                continue;
            const Vector3 hit = positions[parent] + (positions[child] - positions[parent]) * fraction;
            auto& candidate = candidates[parent];
            if (candidate.NodeId != UINT32_MAX
                && float(LengthSquare(hit - center)) >= float(LengthSquare(candidate.Intersection - center)))
                continue;
            candidate = { parent, child, hit, side };
        }

        // 원본 계층 전체를 따라 교차한 조상이 있으면 자손 P를 제거한다.
        std::vector<CrossingJoint> result;
        for (const auto& candidate : candidates)
        {
            if (candidate.NodeId == UINT32_MAX)
                continue;
            uint32_t ancestor = parents[candidate.NodeId];
            bool covered = false;
            size_t depth = 0;
            while (ancestor != UINT32_MAX)
            {
                if (ancestor >= parents.size() || depth++ >= parents.size())
                    return {};
                covered = covered || candidates[ancestor].NodeId != UINT32_MAX;
                ancestor = parents[ancestor];
            }
            if (!covered)
                result.push_back(candidate);
        }
        return result;
    }

    // 현재 월드 위치와 유효 부모에서 유한 평면을 지나는 segment의 부모 본을 고른다.
    inline SegmentJointSelection SelectSegmentJoint(const std::vector<uint32_t>& parents,
        const std::vector<Math::Vector3>& positions, const std::vector<std::vector<uint32_t>>& palettes,
        const MeshCutting::CutPlaneData& plane, const std::vector<uint8_t>& influencing)
    {
        // 무한 평면 선택기와 같은 유효 선분 계층을 준비한다.
        using namespace Math;
        SegmentJointSelection result;
        std::vector<uint32_t> skinParents;
        std::vector<bool> hasChild;
        if (positions.size() != parents.size()
            || !BuildSegmentJointParents(parents, palettes, influencing, skinParents, hasChild))
            return result;
        std::vector<bool> skin(parents.size(), false);
        for (const auto& palette : palettes)
            for (uint32_t node : palette)
                skin[node] = influencing[node] != 0;

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
            if (std::abs(a) <= 1e-6f)
                a = 0;
            if (std::abs(b) <= 1e-6f)
                b = 0;
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
