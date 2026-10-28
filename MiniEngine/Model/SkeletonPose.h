#pragma once

#include "ModelPose.h"
#include <vector>

using NodeId = uint32_t; // invalid = UINT32_MAX

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
