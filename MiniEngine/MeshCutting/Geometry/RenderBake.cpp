#include "../pch.h"
#include "RenderBake.h"

#include <algorithm>
#include <cmath>

namespace MeshCutting
{
    // ---- Render data: the piece as a ProceduralMesh, and its per-frame position update ----

    void BuildRenderMesh(const PatchPiece& piece, ProceduralMeshData& out, const RenderBakeOptions& options)
    {
        out = ProceduralMeshData();
        if (piece.faces.empty())
            return;
        out.vertices.reserve(piece.corners.size());
        for (const RenderCorner& corner : piece.corners)
        {
            ProceduralVertex vertex;
            vertex.position = piece.nodes[corner.node].position;
            vertex.normal = corner.normal;
            vertex.uv = corner.uv;
            vertex.color = corner.color;
            out.vertices.push_back(vertex);
        }
        // Passes: surface (all, or never divided), divided surface (only with splitByOrigin), cap.
        for (int pass = 0; pass < 3; ++pass)
        {
            if (pass == 1 && !options.splitByOrigin)
                continue;
            const FaceKind kind = pass == 2 ? FaceKind::Cap : FaceKind::Surface;
            const uint32_t first = uint32_t(out.indices.size());
            for (const Face& face : piece.faces)
            {
                if (face.kind != kind)
                    continue;
                if (options.splitByOrigin && kind == FaceKind::Surface && (face.cutId != 0) != (pass == 1))
                    continue;
                out.indices.insert(out.indices.end(), { face.corner[0], face.corner[1], face.corner[2] });
            }
            const uint32_t count = uint32_t(out.indices.size()) - first;
            if (count > 0)
                out.sections.push_back({ first, count, pass == 2 ? 1u : 0u });
        }
    }

    bool UpdateRenderPositions(const PatchPiece& piece, std::vector<ProceduralVertex>& vertices)
    {
        if (vertices.size() != piece.corners.size())
            return false;
        for (size_t i = 0; i < vertices.size(); ++i)
        {
            if (piece.corners[i].node >= piece.nodes.size())
                return false;
        }
        for (size_t i = 0; i < vertices.size(); ++i)
            vertices[i].position = piece.nodes[piece.corners[i].node].position;
        return true;
    }

    // ---- Debug shapes: thin tubes for boundaries, loops and the cut plane (CutDebugView bakes them) ----

    DirectX::XMFLOAT4 EdgeKindColor(EdgeKind kind)
    {
        switch (kind)
        {
        case EdgeKind::Replacement: return { 1.0f, 0.95f, 0.0f, 1.0f };     // yellow
        case EdgeKind::UnweldedSeam: return { 0.1f, 1.0f, 0.1f, 1.0f };    // green
        case EdgeKind::Cut: return { 1.0f, 0.45f, 0.0f, 1.0f };            // orange
        case EdgeKind::CapSeam: return { 1.0f, 1.0f, 1.0f, 1.0f };         // white
        case EdgeKind::Interior: break;
        }
        return { 0.5f, 0.5f, 0.5f, 1.0f };
    }

    void AppendDebugSegment(ProceduralMeshData& out, const DirectX::XMFLOAT3& a,
        const DirectX::XMFLOAT3& b, float radius, const DirectX::XMFLOAT4& color)
    {
        double d[3] = { double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z };
        const double length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(length > 0.0) || !std::isfinite(length))
            return;
        for (double& c : d)
            c /= length;

        // Any axis not parallel to the segment gives a perpendicular basis.
        const int least = std::fabs(d[0]) <= std::fabs(d[1]) && std::fabs(d[0]) <= std::fabs(d[2]) ? 0
            : (std::fabs(d[1]) <= std::fabs(d[2]) ? 1 : 2);
        double axis[3] = { 0.0, 0.0, 0.0 };
        axis[least] = 1.0;
        double u[3] = { d[1] * axis[2] - d[2] * axis[1], d[2] * axis[0] - d[0] * axis[2], d[0] * axis[1] - d[1] * axis[0] };
        const double ulen = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (double& c : u)
            c /= ulen;
        const double v[3] = { d[1] * u[2] - d[2] * u[1], d[2] * u[0] - d[0] * u[2], d[0] * u[1] - d[1] * u[0] };

        const uint32_t first = uint32_t(out.vertices.size());
        const double angle[3] = { 0.0, 2.0943951023931953, 4.1887902047863905 };
        for (int end = 0; end < 2; ++end)
        {
            const DirectX::XMFLOAT3& p = end == 0 ? a : b;
            for (int k = 0; k < 3; ++k)
            {
                const double c = std::cos(angle[k]), s = std::sin(angle[k]);
                const double nx = u[0] * c + v[0] * s, ny = u[1] * c + v[1] * s, nz = u[2] * c + v[2] * s;
                ProceduralVertex vertex;
                vertex.position = { float(p.x + radius * nx), float(p.y + radius * ny), float(p.z + radius * nz) };
                vertex.normal = { float(nx), float(ny), float(nz) };
                vertex.uv = { 0.0f, 0.0f };
                vertex.color = color;
                out.vertices.push_back(vertex);
            }
        }
        for (uint32_t k = 0; k < 3; ++k)
        {
            const uint32_t k2 = (k + 1) % 3;
            const uint32_t a0 = first + k, a1 = first + k2, b0 = first + 3 + k, b1 = first + 3 + k2;
            out.indices.insert(out.indices.end(), { a0, a1, b1, a0, b1, b0 });
        }
    }

    void BakeBoundaryDebug(const PatchPiece& piece, float radius, ProceduralMeshData& out)
    {
        out = ProceduralMeshData();
        std::vector<uint64_t> keys;
        keys.reserve(piece.boundary.size());
        for (const auto& entry : piece.boundary)
        {
            // CapSeam edges are interior; they are tagged but not drawn as open boundary.
            if (entry.second != EdgeKind::CapSeam)
                keys.push_back(entry.first);
        }
        std::sort(keys.begin(), keys.end());
        for (uint64_t key : keys)
        {
            const uint32_t a = EdgeKeyLow(key), b = EdgeKeyHigh(key);
            if (a >= piece.nodes.size() || b >= piece.nodes.size())
                continue;
            AppendDebugSegment(out, piece.nodes[a].position, piece.nodes[b].position, radius,
                EdgeKindColor(piece.boundary.at(key)));
        }
        if (!out.indices.empty())
            out.sections.push_back({ 0, uint32_t(out.indices.size()), 0 });
    }

    void BakeLoopDebug(const PatchPiece& piece, const std::vector<std::vector<uint32_t>>& loops, float radius,
        const DirectX::XMFLOAT4& color, ProceduralMeshData& out)
    {
        out = ProceduralMeshData();
        for (const std::vector<uint32_t>& loop : loops)
        {
            for (size_t i = 0; i < loop.size(); ++i)
            {
                const uint32_t a = loop[i], b = loop[(i + 1) % loop.size()];
                if (a < piece.nodes.size() && b < piece.nodes.size())
                    AppendDebugSegment(out, piece.nodes[a].position, piece.nodes[b].position, radius, color);
            }
        }
        if (!out.indices.empty())
            out.sections.push_back({ 0, uint32_t(out.indices.size()), 0 });
    }

    void BakeCutPlaneDebug(const CutPlaneFrame& frame, float tubeRadius, const DirectX::XMFLOAT4& color,
        ProceduralMeshData& out)
    {
        out = ProceduralMeshData();
        const DirectX::XMFLOAT4 arrowColor = { 1.0f, 1.0f, 1.0f, 1.0f };
        const float hx = frame.halfExtents.x, hy = frame.halfExtents.y;
        const auto at = [&](float a, float b, float c)
        {
            return DirectX::XMFLOAT3(frame.center.x + a * frame.axisU.x + b * frame.axisV.x + c * frame.normal.x,
                frame.center.y + a * frame.axisU.y + b * frame.axisV.y + c * frame.normal.y,
                frame.center.z + a * frame.axisU.z + b * frame.axisV.z + c * frame.normal.z);
        };

        // The rectangle border.
        AppendDebugSegment(out, at(-hx, -hy, 0.0f), at(hx, -hy, 0.0f), tubeRadius, color);
        AppendDebugSegment(out, at(hx, -hy, 0.0f), at(hx, hy, 0.0f), tubeRadius, color);
        AppendDebugSegment(out, at(hx, hy, 0.0f), at(-hx, hy, 0.0f), tubeRadius, color);
        AppendDebugSegment(out, at(-hx, hy, 0.0f), at(-hx, -hy, 0.0f), tubeRadius, color);

        // Cross lines inside: the two center lines and one at half the way to each side, thinner.
        const float fractions[3] = { -0.5f, 0.0f, 0.5f };
        for (float f : fractions)
        {
            AppendDebugSegment(out, at(-hx, f * hy, 0.0f), at(hx, f * hy, 0.0f), tubeRadius * 0.5f, color);
            AppendDebugSegment(out, at(f * hx, -hy, 0.0f), at(f * hx, hy, 0.0f), tubeRadius * 0.5f, color);
        }

        // The arrow points to the positive side (the side the plane normal points to).
        const float length = std::min(hx, hy) * 0.7f;
        const float head = length * 0.15f;
        AppendDebugSegment(out, at(0.0f, 0.0f, 0.0f), at(0.0f, 0.0f, length), tubeRadius, arrowColor);
        AppendDebugSegment(out, at(0.0f, 0.0f, length), at(head, 0.0f, length - 2.0f * head), tubeRadius, arrowColor);
        AppendDebugSegment(out, at(0.0f, 0.0f, length), at(-head, 0.0f, length - 2.0f * head), tubeRadius, arrowColor);
        AppendDebugSegment(out, at(0.0f, 0.0f, length), at(0.0f, head, length - 2.0f * head), tubeRadius, arrowColor);
        AppendDebugSegment(out, at(0.0f, 0.0f, length), at(0.0f, -head, length - 2.0f * head), tubeRadius, arrowColor);
        if (!out.indices.empty())
            out.sections.push_back({ 0, uint32_t(out.indices.size()), 0 });
    }
}
