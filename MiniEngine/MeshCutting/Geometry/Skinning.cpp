// 이 파일은 스킨 정점 표로 piece를 현재 포즈에 맞추는 계산(Skinning::Apply)과 joint 집합 판정을 구현한다.
#include "../pch.h"
#include "Skinning.h"

#include <cmath>

using namespace DirectX;

namespace MeshCutting
{
    namespace
    {
        bool Fail(std::string* error, const std::string& message)
        {
            if (error)
                *error = message;
            return false;
        }
    }

    XMVECTOR Skinning::SkinPoint(const XMFLOAT4X4* matrices, uint32_t matrixCount, const uint16_t* joints,
        const float* weights, uint32_t entryCount, const XMFLOAT3& bindPosition)
    {
        const XMVECTOR bind = XMVectorSet(bindPosition.x, bindPosition.y, bindPosition.z, 1.0f);
        XMVECTOR sum = XMVectorZero();
        for (uint32_t i = 0; i < entryCount; ++i)
        {
            if (weights[i] <= 0.0f || joints[i] >= matrixCount)
                continue;
            const XMMATRIX m = XMLoadFloat4x4(&matrices[joints[i]]);
            sum = XMVectorMultiplyAdd(XMVector4Transform(bind, m), XMVectorReplicate(weights[i]), sum);
        }
        return sum;
    }

    bool Skinning::Apply(PatchPiece& piece, const std::vector<std::vector<XMFLOAT4X4>>& slotMatrices,
        SkinStats* stats, std::string* error) const
    {
        if (stats)
            *stats = SkinStats();
        if (error)
            error->clear();
        if (piece.skinStateId != 0)
            return Fail(error, "piece has skin state " + Num(piece.skinStateId) + "; separated skinning is not supported yet");

        // 쓰기 전에 piece가 쓰는 스킨 정점을 모두 검사한다.
        std::vector<uint8_t> needed(m_Vertices.size(), 0);
        for (size_t n = 0; n < piece.nodes.size(); ++n)
        {
            const PatchNode& node = piece.nodes[n];
            if (node.kind != PatchNodeKind::Surface)
                continue;
            if (node.origin.skinVertex[0] == kNoNode)
                return Fail(error, "node " + Num(n) + " has no skin vertex");
            for (int k = 0; k < 3 && node.origin.skinVertex[k] != kNoNode; ++k)
            {
                if (node.origin.skinVertex[k] >= m_Vertices.size())
                    return Fail(error, "node " + Num(n) + " refers to skin vertex " + Num(node.origin.skinVertex[k])
                        + " beyond " + Num(m_Vertices.size()));
                needed[node.origin.skinVertex[k]] = 1;
            }
        }

        std::vector<XMFLOAT3> blend(m_Vertices.size());
        uint32_t evaluated = 0;
        for (size_t o = 0; o < m_Vertices.size(); ++o)
        {
            if (!needed[o])
                continue;
            const SkinVertex& skin = m_Vertices[o];
            if (skin.meshSlot >= slotMatrices.size())
                return Fail(error, "skin vertex " + Num(o) + " uses mesh slot " + Num(skin.meshSlot)
                    + " but only " + Num(slotMatrices.size()) + " slots have matrices");
            if (skin.jointCount == 0 || skin.jointCount > 4)
                return Fail(error, "skin vertex " + Num(o) + " has " + Num(skin.jointCount) + " joints");
            const std::vector<XMFLOAT4X4>& matrices = slotMatrices[skin.meshSlot];
            for (uint32_t j = 0; j < skin.jointCount; ++j)
                if (skin.joints[j] >= matrices.size())
                    return Fail(error, "skin vertex " + Num(o) + " uses joint " + Num(skin.joints[j])
                        + " but slot " + Num(skin.meshSlot) + " has " + Num(matrices.size()) + " matrices");
            XMStoreFloat3(&blend[o], SkinPoint(matrices.data(), uint32_t(matrices.size()), skin.joints, skin.weights,
                skin.jointCount, skin.bindPosition));
            if (!std::isfinite(blend[o].x) || !std::isfinite(blend[o].y) || !std::isfinite(blend[o].z))
                return Fail(error, "skin vertex " + Num(o) + " blends to a non-finite position");
            ++evaluated;
        }

        // 정점 위치 = 스킨 정점 혼합의 bary 가중합
        std::vector<XMFLOAT3> positions(piece.nodes.size());
        uint32_t written = 0, skipped = 0;
        for (size_t n = 0; n < piece.nodes.size(); ++n)
        {
            const PatchNode& node = piece.nodes[n];
            if (node.kind != PatchNodeKind::Surface)
            {
                positions[n] = node.position;
                ++skipped;
                continue;
            }
            XMVECTOR sum = XMVectorZero();
            for (int k = 0; k < 3 && node.origin.skinVertex[k] != kNoNode; ++k)
                sum = XMVectorMultiplyAdd(XMLoadFloat3(&blend[node.origin.skinVertex[k]]), XMVectorReplicate(node.origin.bary[k]), sum);
            XMStoreFloat3(&positions[n], sum);
            if (!std::isfinite(positions[n].x) || !std::isfinite(positions[n].y) || !std::isfinite(positions[n].z))
                return Fail(error, "node " + Num(n) + " ends up at a non-finite position");
            ++written;
        }

        for (size_t n = 0; n < piece.nodes.size(); ++n)
            piece.nodes[n].position = positions[n];
        if (stats)
        {
            stats->skinVerticesEvaluated = evaluated;
            stats->surfaceVerticesWritten = written;
            stats->capVerticesSkipped = skipped;
        }
        return true;
    }

    float Skinning::Influence(const uint16_t joints[4], const float weights[4], uint32_t joint)
    {
        float influence = 0.0f;
        for (int i = 0; i < 4; ++i)
            if (joints[i] == joint)
                influence += weights[i];
        return influence;
    }

    float Skinning::SetInfluence(const uint16_t joints[4], const float weights[4], const std::vector<uint32_t>& jointSet)
    {
        float influence = 0.0f;
        for (uint32_t joint : jointSet)
            influence += Influence(joints, weights, joint);
        return influence;
    }

    bool Skinning::InJointSet(const uint16_t joints[4], const float weights[4], const std::vector<uint32_t>& jointSet,
        float minWeight)
    {
        for (uint32_t joint : jointSet)
            if (Influence(joints, weights, joint) > minWeight)
                return true;
        return false;
    }

    bool Skinning::IsValidJointSet(const std::vector<uint32_t>& jointSet, uint32_t jointCount)
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
