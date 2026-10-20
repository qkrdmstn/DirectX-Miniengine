#pragma once

#include "PatchGeometry.h"

namespace MeshCutting
{
    // 바인드 포즈에서의 정점 하나를 영향 받는 joint에 대한 가중치를 선형으로 적용해 현재 월드 좌표계에서의 위치로 반환한다.
    DirectX::XMVECTOR SkinPoint(const DirectX::XMFLOAT4X4* matrices, uint32_t matrixCount,
        const uint16_t* joints, const float* weights, uint32_t entryCount,
        const DirectX::XMFLOAT3& bindPosition);

    struct SkinStats
    {
        uint32_t originNodesEvaluated = 0;      // linear blends computed (each at most once per call)
        uint32_t surfaceNodesWritten = 0;
        uint32_t capNodesSkipped = 0;           // Cap-kind nodes keep their position (none exist while caps share rim nodes)
    };

    // Moves every node of a piece to the current pose (design 4). slotMatrices[meshSlot][joint]
    // holds the joint transforms of each source mesh.
    //
    //  - Original nodes: sum of weight * M_joint * bindPosition, joints in ascending order.
    //  - Cut nodes: sum of bary * blend(origin node), used slots only, in ascending node order.
    //    Bone ids and weights are never interpolated; a cut node is driven by its SurfaceOrigin.
    //  - Cap nodes are not computed. A cap shares the rim nodes of the surface, so it follows
    //    them without any extra work.
    //  - The blend of an origin node is computed once per call and reused by every node
    //    that refers to it.
    //
    // Fails (false, detail filled, piece untouched) for a piece with skinStateId != 0 (separated
    // bones are a later stage), an origin node or joint that is out of range, or a non-finite result.
    bool SkinPatchPiece(PatchPiece& piece, const std::vector<std::vector<DirectX::XMFLOAT4X4>>& slotMatrices,
        SkinStats* stats = nullptr, std::string* detail = nullptr);
}
