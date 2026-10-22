#pragma once

// 이 파일은 패치 조각을 ProceduralMesh로 만드는 렌더 데이터와 디버그용 튜브 모양을 만든다. Core에는 의존하지 않는다.

#include "../Geometry/Patch.h"
#include "../ProceduralMesh.h"

namespace MeshCutting
{
    // ---- Render data: the piece as a ProceduralMesh, and its per-frame position update ----

    struct RenderBakeOptions
    {
        // Debug: split the surface faces into [never divided][divided by a cut] sections. Both keep
        // materialSlot 0; the cap sections stay last.
        bool splitByOrigin = false;
    };

    // Piece -> render mesh. Vertex i is corner i with its node's position. The surface section(s)
    // come first with materialSlot 0, then one cap section per PatchCap::materialSlot (default 1).
    // Cap faces of a cap with inLastSection go into the last surface section instead. An empty
    // section is not created. out is cleared first.
    void BuildRenderMesh(const PatchPiece& piece, ProceduralMeshData& out, const RenderBakeOptions& options = {});

    // Copies the node position of every corner into vertices[corner]. This is the per-frame update
    // of a baked piece: topology, UVs, normals and colors stay as they were (normals are not
    // recomputed). Returns false and leaves vertices alone when the sizes differ.
    bool UpdateRenderPositions(const PatchPiece& piece, std::vector<ProceduralVertex>& vertices);

    // ---- Debug shapes: thin tubes for boundaries, loops and the cut plane (CutDebugView bakes them) ----
    // They stay here, not in Debug/, because they are plain geometry with no D3D12 dependency and the
    // standalone tests (tests/PlaneSlicerTests.cpp) build only Geometry/.

    // Debug colors of tagged edges (Interior has none).
    DirectX::XMFLOAT4 EdgeKindColor(PatchEdgeKind kind);

    // Appends a thin three-sided tube from a to b. Two-sided shading makes winding irrelevant.
    void AppendDebugSegment(ProceduralMeshData& out, const DirectX::XMFLOAT3& a,
        const DirectX::XMFLOAT3& b, float radius, const DirectX::XMFLOAT4& color);

    // One tube per tagged boundary edge, colored by kind. Edges are visited in key order so the
    // output does not depend on hash order. out is cleared first.
    void BakeBoundaryDebug(const PatchPiece& piece, float radius, ProceduralMeshData& out);

    // The cut loops of a slice result (positive piece numbering) as closed rings of thin tubes, so the
    // cut can be seen before it is confirmed. out is cleared.
    void BakeLoopDebug(const PatchPiece& piece, const std::vector<std::vector<uint32_t>>& loops, float radius,
        const DirectX::XMFLOAT4& color, ProceduralMeshData& out);
}
