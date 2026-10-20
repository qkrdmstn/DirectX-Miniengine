#include "../pch.h"
#include "JointSet.h"

namespace MeshCutting
{
    float JointInfluence(const uint16_t joints[4], const float weights[4], uint32_t joint)
    {
        float influence = 0.0f;
        for (int i = 0; i < 4; ++i)
            if (joints[i] == joint)
                influence += weights[i];
        return influence;
    }

    float JointSetInfluence(const uint16_t joints[4], const float weights[4], const std::vector<uint32_t>& jointSet)
    {
        float influence = 0.0f;
        for (uint32_t joint : jointSet)
            influence += JointInfluence(joints, weights, joint);
        return influence;
    }

    bool VertexInJointSet(const uint16_t joints[4], const float weights[4], const std::vector<uint32_t>& jointSet,
        float minWeight)
    {
        for (uint32_t joint : jointSet)
            if (JointInfluence(joints, weights, joint) > minWeight)
                return true;
        return false;
    }

    bool IsValidJointSet(const std::vector<uint32_t>& jointSet, uint32_t jointCount)
    {
        if (jointSet.empty())
            return false;
        for (size_t i = 0; i < jointSet.size(); ++i)
        {
            if (jointSet[i] >= jointCount || (i > 0 && jointSet[i] <= jointSet[i - 1]))
                return false;
        }
        return true;
    }
}
