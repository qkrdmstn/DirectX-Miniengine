#pragma once

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

    // Piece -> render mesh. Vertex i is corner i with its node's position. Section 0 holds the
    // surface faces and section 1 the cap faces; an empty section is not created. out is
    // cleared first.
    void BuildRenderMesh(const PatchPiece& piece, ProceduralMeshData& out);

    // A ring in the plane (radius ringRadius) plus a normal arrow, as thin tubes. out is cleared.
    void BakePlaneDebug(const SlicePlane& plane, float ringRadius, float tubeRadius, ProceduralMeshData& out);
}
