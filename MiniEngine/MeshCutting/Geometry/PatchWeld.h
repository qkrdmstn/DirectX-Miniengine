#pragma once

#include "PatchGeometry.h"

namespace MeshCutting
{
    // One render vertex of an extracted patch, as plain data (no Model or Core types).
    struct WeldInputVertex
    {
        uint32_t meshSlot = 0;
        DirectX::XMFLOAT3 bindPosition = { 0.0f, 0.0f, 0.0f };  // pre-skinning position
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };      // posed position at extraction
        DirectX::XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
        DirectX::XMFLOAT2 uv = { 0.0f, 0.0f };
        DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
        uint16_t joints[4] = {};
        float weights[4] = {};
    };

    struct WeldInput
    {
        std::vector<WeldInputVertex> vertices;
        std::vector<uint32_t> indices;      // triangle list; triangle i is faces[i]
    };

    struct WeldStats
    {
        uint32_t corners = 0;               // render vertices before welding
        uint32_t nodes = 0;                 // geometry nodes after welding
        uint32_t positionNodes = 0;         // nodes if skin equality were ignored (comparison only)
        uint32_t skinSplitPositions = 0;    // bind positions that ended up with more than one node
        uint32_t faces = 0;
        uint32_t boundaryEdges = 0;
        uint32_t replacementEdges = 0;
        uint32_t unweldedSeamEdges = 0;     // counts both sides of a seam
        uint32_t nonManifoldEdges = 0;
    };

    // Welds render vertices into geometry nodes. The key is (mesh slot, bind position bits,
    // skin pairs sorted by joint); there is no epsilon welding. Boundary edges are tagged
    // Replacement, or UnweldedSeam when another boundary edge covers the same bind positions
    // (same slot) or the same posed positions (different slots).
    //
    // stats is filled as far as the build got, even on failure. On failure out is empty.
    SliceStatus BuildPatchPiece(const WeldInput& in, float epsilonScale, PatchPiece& out,
        WeldStats& stats, std::string* detail = nullptr);
}
