#pragma once

#include "CutPlane.h"
#include "PatchGeometry.h"
#include "../ProceduralMesh.h"

namespace MeshCutting
{
    // Debug colors of tagged edges (Interior has none).
    DirectX::XMFLOAT4 EdgeKindColor(EdgeKind kind);

    // Appends a thin three-sided tube from a to b. Two-sided shading makes winding irrelevant.
    void AppendDebugSegment(ProceduralMeshData& out, const DirectX::XMFLOAT3& a,
        const DirectX::XMFLOAT3& b, float radius, const DirectX::XMFLOAT4& color);

    // One tube per tagged boundary edge, colored by kind. Edges are visited in key order so the
    // output does not depend on hash order. out is cleared first.
    void BakeBoundaryDebug(const PatchPiece& piece, float radius, ProceduralMeshData& out);

    struct RenderBakeOptions
    {
        // Debug: split the surface faces into [never divided][divided by a cut] sections. Both keep
        // materialSlot 0; the cap section stays last with materialSlot 1.
        bool splitByOrigin = false;
    };

    // Piece -> render mesh. Vertex i is corner i with its node's position. Section 0 holds the
    // surface faces and section 1 the cap faces; an empty section is not created. out is
    // cleared first.
    void BuildRenderMesh(const PatchPiece& piece, ProceduralMeshData& out, const RenderBakeOptions& options = {});

    // Copies the node position of every corner into vertices[corner]. This is the per-frame update
    // of a baked piece: topology, UVs, normals and colors stay as they were (normals are not
    // recomputed). Returns false and leaves vertices alone when the sizes differ.
    bool UpdateRenderPositions(const PatchPiece& piece, std::vector<ProceduralVertex>& vertices);

    // The cut plane as thin tubes: the rectangle border and a few cross lines in color, plus a white
    // arrow along the normal. out is cleared.
    void BakeCutPlaneDebug(const CutPlaneFrame& frame, float tubeRadius, const DirectX::XMFLOAT4& color,
        ProceduralMeshData& out);

    // The cut loops of a slice result (positive piece numbering) as closed rings of thin tubes, so the
    // cut can be seen before it is confirmed. out is cleared.
    void BakeLoopDebug(const PatchPiece& piece, const std::vector<std::vector<uint32_t>>& loops, float radius,
        const DirectX::XMFLOAT4& color, ProceduralMeshData& out);
}
