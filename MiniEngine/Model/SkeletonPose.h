#pragma once

#include "ModelPose.h"
#include <vector>

using NodeId = uint32_t; // invalid = UINT32_MAX

struct NodePhysicsPose
{
    NodeId Node;
    Math::Matrix4 ModelPose;
};

// 기존 pose 평가기의 DFS 규칙으로 matrixIdx 기준 유효 부모를 복원한다.
// 검증에 실패하면 호출자의 부모 배열을 그대로 보존한다.
inline bool BuildPoseParents(const GraphNode* graph, uint32_t count,
    std::vector<NodeId>& effectiveParents)
{
    // 빈 입력을 거절하고 임시 결과와 중복 검사 배열을 준비한다.
    if (!graph || count == 0)
        return false;
    std::vector<NodeId> parents(count, UINT32_MAX);
    std::vector<bool> visited(count, false);
    NodeId stack[32];
    uint32_t depth = 0;
    NodeId parent = UINT32_MAX;

    // DFS 위치와 별개인 matrixIdx로 부모를 기록한다.
    for (uint32_t i = 0; i < count; ++i)
    {
        // 범위와 유일성을 확인한 뒤 skeletonRoot의 부모 공간을 끊는다.
        const GraphNode& node = graph[i];
        if (node.matrixIdx >= count || visited[node.matrixIdx])
            return false;
        visited[node.matrixIdx] = true;
        parents[node.matrixIdx] = node.skeletonRoot ? UINT32_MAX : parent;

        // 평가기처럼 자식과 형제가 함께 있을 때만 복귀할 부모를 저장한다.
        if (node.hasChildren)
        {
            // 고정 크기 형제 스택의 한계를 평가기와 동일하게 검사한다.
            if (node.hasSibling)
            {
                if (depth == 32)
                    return false;
                stack[depth++] = parent;
            }
            parent = node.matrixIdx;
        }
        else if (!node.hasSibling)
        {
            // 모든 입력을 소비한 정상 종료에서만 결과를 공개한다.
            if (depth == 0)
            {
                if (i + 1 != count)
                    return false;
                effectiveParents.swap(parents);
                return true;
            }
            parent = stack[--depth];
        }
    }

    // 자식이나 형제 또는 스택 복귀가 남은 입력은 비정상 종료다.
    return false;
}

// 유효 부모 순서로 애니메이션과 물리 model pose를 합성한다.
// 모든 검증과 계산이 성공한 경우에만 호출자의 출력 배열을 갱신한다.
inline bool EvaluatePoseOverrides(const GraphNode* graph, uint32_t count,
    const std::vector<NodeId>& effectiveParents,
    const NodePhysicsPose* overrides, uint32_t overrideCount,
    Math::Matrix4* finalModelPose)
{
    // 입력 포인터와 부모 배열이 원본 DFS 구조에 일치하는지 확인한다.
    if (!graph || !finalModelPose || count == 0 || effectiveParents.size() != count
        || (overrideCount != 0 && !overrides))
        return false;
    std::vector<NodeId> parents;
    if (!BuildPoseParents(graph, count, parents) || parents != effectiveParents)
        return false;
    std::vector<uint32_t> overrideIndices(count, UINT32_MAX);

    // 중복 또는 범위 밖 식별자와 비유한 물리 행렬을 거절한다.
    for (uint32_t i = 0; i < overrideCount; ++i)
    {
        // SIMD 검사로 행렬 전체의 NaN과 무한대를 확인한다.
        const NodePhysicsPose& pose = overrides[i];
        if (pose.Node >= count || overrideIndices[pose.Node] != UINT32_MAX
            || DirectX::XMMatrixIsNaN(pose.ModelPose) || DirectX::XMMatrixIsInfinite(pose.ModelPose))
            return false;
        overrideIndices[pose.Node] = i;
    }

    // DFS의 부모 우선 순서에서 물리 pose 또는 부모와 local의 합성을 선택한다.
    std::vector<Math::Matrix4> result(count);
    const Math::Matrix4 identity(Math::kIdentity);
    for (uint32_t i = 0; i < count; ++i)
    {
        // 물리 노드는 전달받은 model pose를 그대로 보존한다.
        const GraphNode& node = graph[i];
        const NodeId id = node.matrixIdx;
        const uint32_t overrideIndex = overrideIndices[id];
        if (overrideIndex != UINT32_MAX)
        {
            // 물리 부모 아래의 비물리 자손은 이후 반복에서 local을 합성한다.
            result[id] = overrides[overrideIndex].ModelPose;
        }
        else
        {
            // 일반 최상위 노드도 기존 평가기와 동일한 identity 곱셈을 거친다.
            const NodeId parent = effectiveParents[id];
            result[id] = node.skeletonRoot ? node.xform
                : (parent == UINT32_MAX ? identity : result[parent]) * node.xform;
        }
    }

    // 실패 가능성이 사라진 뒤에만 임시 결과를 출력으로 복사한다.
    for (uint32_t i = 0; i < count; ++i)
    {
        // matrixIdx 기준 배열의 모든 원소를 게시한다.
        finalModelPose[i] = result[i];
    }
    return true;
}
