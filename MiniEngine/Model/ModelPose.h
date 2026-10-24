#pragma once

#include "VectorMath.h"
#include <cstdint>

struct GraphNode // Serialized layout: 96 bytes. Keep field order and bit widths unchanged.
{
    Math::Matrix4 xform;
    Math::Quaternion rotation;
    Math::XMFLOAT3 scale;
    uint32_t matrixIdx : 28;
    uint32_t hasSibling : 1;
    uint32_t hasChildren : 1;
    uint32_t staleMatrix : 1;
    uint32_t skeletonRoot : 1;
};
static_assert(sizeof(GraphNode) == 96, "GraphNode serialized layout changed");

// Evaluate the loader's depth-first graph without an instance/world transform.
// skeletonRoot preserves the loader's intentional parent-space reset.
inline bool EvaluateModelPose(const GraphNode* nodes, uint32_t count, Math::Matrix4* transforms)
{
    if (!nodes || !transforms || count == 0)
        return false;
    Math::Matrix4 parents[32];
    uint32_t depth = 0;
    Math::Matrix4 parent(Math::kIdentity);
    for (uint32_t i = 0; i < count; ++i)
    {
        const GraphNode& node = nodes[i];
        if (node.matrixIdx >= count)
            return false;
        const Math::Matrix4 pose = node.skeletonRoot ? node.xform : parent * node.xform;
        transforms[node.matrixIdx] = pose;
        if (node.hasChildren)
        {
            if (node.hasSibling)
            {
                if (depth == 32)
                    return false;
                parents[depth++] = parent;
            }
            parent = pose;
        }
        else if (!node.hasSibling)
        {
            if (depth == 0)
                return i + 1 == count;
            parent = parents[--depth];
        }
    }
    return false;
}
