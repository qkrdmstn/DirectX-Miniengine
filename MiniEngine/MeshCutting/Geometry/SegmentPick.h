#pragma once

// 절단 평면이 지나는 뼈 선분을 찾는다. 설계: Docs/PlaneFirstCutDesign.md 3절. Model, Core에 의존하지 않는다.

#include "CutPlane.h"
#include "PatchGeometry.h"
#include <vector>

namespace MeshCutting
{
    // 한 팔레트의 뼈대를 어떤 자세에서 본 모습. 패치 정점과 같은 공간(BonePatchSource::poseJointWorld와
    // 같은 행렬 규약)이다.
    struct SkeletonPose
    {
        std::vector<DirectX::XMFLOAT3> jointPositions;
        std::vector<uint32_t> parents;      // 팔레트 로컬 부모 joint, 없으면 kNoNode
    };

    // 평면이 지나는 부모 -> 자식 뼈 선분 하나.
    struct SegmentHit
    {
        uint32_t parentJoint = kNoNode;     // 자를 joint (선분의 부모 쪽 끝)
        uint32_t childJoint = kNoNode;
        float t = 0.0f;                     // 부모 0 -> 자식 1
        DirectX::XMFLOAT3 point = { 0.0f, 0.0f, 0.0f };     // 선분과 평면이 만나는 점
    };

    // 선분마다 양 끝이 평면의 어느 쪽인지 본다. 부호 거리가 0 이상이면 + 쪽이므로, 평면이 joint를 정확히
    // 지나도 사슬의 선분 하나에만 걸린다. 교차점이 사각형 안(ContainsInPlane, margin 0)인 선분만 돌려주고,
    // 순서는 childJoint 오름차순이다. 평면이 올바르지 않거나 jointPositions와 parents 크기가 다르면 hits는 비어 있다.
    void PickSegments(const SkeletonPose& pose, const CutPlaneFrame& plane, std::vector<SegmentHit>& hits);

    // 절단 joint 집합: hits의 parentJoint를 중복 없이 오름차순으로.
    std::vector<uint32_t> CutJoints(const std::vector<SegmentHit>& hits);
}
