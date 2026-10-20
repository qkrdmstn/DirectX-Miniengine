#include "../pch.h"
#include "PieceSkinning.h"

#include <cmath>

using namespace DirectX;

namespace MeshCutting
{
    namespace
    {
        bool Fail(std::string* detail, const std::string& message)
        {
            if (detail)
                *detail = message;
            return false;
        }

        std::string Num(uint64_t value) { return std::to_string(value); }
    }

    XMVECTOR SkinPoint(const XMFLOAT4X4* matrices, uint32_t matrixCount, const uint16_t* joints,
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

    bool SkinPatchPiece(PatchPiece& piece, const std::vector<std::vector<XMFLOAT4X4>>& slotMatrices,
        SkinStats* stats, std::string* detail)
    {
        if (stats)
            *stats = SkinStats();
        if (detail)
            detail->clear();
        if (piece.skinStateId != 0)
            return Fail(detail, "piece has skin state " + Num(piece.skinStateId) + "; separated skinning is not supported yet");

        // Which origin nodes does this piece need? Check everything before writing anything.
        std::vector<uint8_t> needed(piece.originNodes.size(), 0);
        for (size_t n = 0; n < piece.nodes.size(); ++n)
        {
            const GeomNode& node = piece.nodes[n];
            if (node.kind != NodeKind::Surface)
                continue;
            if (node.origin.node[0] == kNoNode)
                return Fail(detail, "node " + Num(n) + " has no origin node");
            for (int k = 0; k < 3 && node.origin.node[k] != kNoNode; ++k)
            {
                if (node.origin.node[k] >= piece.originNodes.size())
                    return Fail(detail, "node " + Num(n) + " refers to origin node " + Num(node.origin.node[k])
                        + " beyond " + Num(piece.originNodes.size()));
                needed[node.origin.node[k]] = 1;
            }
        }

        std::vector<XMFLOAT3> blend(piece.originNodes.size());
        uint32_t evaluated = 0;
        for (size_t o = 0; o < piece.originNodes.size(); ++o)
        {
            if (!needed[o])
                continue;
            const SkinNode& skin = piece.originNodes[o];
            if (skin.meshSlot >= slotMatrices.size())
                return Fail(detail, "origin node " + Num(o) + " uses mesh slot " + Num(skin.meshSlot)
                    + " but only " + Num(slotMatrices.size()) + " slots have matrices");
            if (skin.jointCount == 0 || skin.jointCount > 4)
                return Fail(detail, "origin node " + Num(o) + " has " + Num(skin.jointCount) + " joints");
            const std::vector<XMFLOAT4X4>& matrices = slotMatrices[skin.meshSlot];
            for (uint32_t j = 0; j < skin.jointCount; ++j)
                if (skin.joints[j] >= matrices.size())
                    return Fail(detail, "origin node " + Num(o) + " uses joint " + Num(skin.joints[j])
                        + " but slot " + Num(skin.meshSlot) + " has " + Num(matrices.size()) + " matrices");
            XMStoreFloat3(&blend[o], SkinPoint(matrices.data(), uint32_t(matrices.size()), skin.joints, skin.weights,
                skin.jointCount, skin.bindPosition));
            if (!std::isfinite(blend[o].x) || !std::isfinite(blend[o].y) || !std::isfinite(blend[o].z))
                return Fail(detail, "origin node " + Num(o) + " blends to a non-finite position");
            ++evaluated;
        }

        // Positions of the nodes: bary-weighted sums over the origin nodes.
        std::vector<XMFLOAT3> positions(piece.nodes.size());
        uint32_t written = 0, skipped = 0;
        for (size_t n = 0; n < piece.nodes.size(); ++n)
        {
            const GeomNode& node = piece.nodes[n];
            if (node.kind != NodeKind::Surface)
            {
                positions[n] = node.position;
                ++skipped;
                continue;
            }
            XMVECTOR sum = XMVectorZero();
            for (int k = 0; k < 3 && node.origin.node[k] != kNoNode; ++k)
                sum = XMVectorMultiplyAdd(XMLoadFloat3(&blend[node.origin.node[k]]), XMVectorReplicate(node.origin.bary[k]), sum);
            XMStoreFloat3(&positions[n], sum);
            if (!std::isfinite(positions[n].x) || !std::isfinite(positions[n].y) || !std::isfinite(positions[n].z))
                return Fail(detail, "node " + Num(n) + " ends up at a non-finite position");
            ++written;
        }

        for (size_t n = 0; n < piece.nodes.size(); ++n)
            piece.nodes[n].position = positions[n];
        if (stats)
        {
            stats->originNodesEvaluated = evaluated;
            stats->surfaceNodesWritten = written;
            stats->capNodesSkipped = skipped;
        }
        return true;
    }
}
