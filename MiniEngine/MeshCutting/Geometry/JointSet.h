#pragma once

// 정점이 joint 집합에 속하는지 판단하는 규칙. 패치 추출(BonePatch)이 쓰고, Model 없이 테스트할 수 있도록
// 따로 뒀다. 설계: Docs/PlaneFirstCutDesign.md 4절.

#include <cstdint>
#include <vector>

namespace MeshCutting
{
    // 정점 하나가 가진 4개 슬롯 중 joint에 대한 가중치의 합 (같은 joint가 여러 슬롯에 있을 수 있다).
    float JointInfluence(const uint16_t joints[4], const float weights[4], uint32_t joint);

    // 집합 안 joint들의 JointInfluence 합. 가중치 색을 칠할 때 쓴다 (정규화된 가중치의 합이라 1을 넘지 않는다).
    float JointSetInfluence(const uint16_t joints[4], const float weights[4], const std::vector<uint32_t>& jointSet);

    // 집합 안 joint 중 하나라도 JointInfluence가 minWeight를 넘으면 참이다. joint마다 따로 판단하므로,
    // 집합의 결과는 각 joint 결과의 합집합이고 크기 1 집합은 그 joint 하나만 쓴 결과와 같다.
    bool VertexInJointSet(const uint16_t joints[4], const float weights[4], const std::vector<uint32_t>& jointSet,
        float minWeight);

    // 오름차순, 중복 없음이고 모든 값이 jointCount 미만이면 참이다. 비어 있으면 거짓이다.
    bool IsValidJointSet(const std::vector<uint32_t>& jointSet, uint32_t jointCount);
}
