// 이 파일은 패치 조각을 ProceduralMesh로 만드는 렌더 데이터와 디버그용 튜브 모양을 구현한다.
#include "../pch.h"
#include "RenderBake.h"

#include <algorithm>
#include <cmath>

namespace MeshCutting
{
    // ---- Render data: the piece as a ProceduralMesh, and its per-frame position update ----

    void BuildRenderMesh(const MeshGeometry& piece, ProceduralMeshData& out, const RenderBakeOptions& options)
    {
        out = ProceduralMeshData();
        if (piece.faces.empty())
            return;
        out.vertices.reserve(piece.corners.size());
        for (const MeshCorner& corner : piece.corners)
        {
            MeshRenderVertex vertex;
            vertex.position = piece.vertices[corner.vertex].position;
            vertex.normal = corner.normal;
            vertex.uv = corner.uv;
            vertex.color = corner.color;
            out.vertices.push_back(vertex);
        }
        // cap 면의 재질 규칙은 같은 cutId의 MeshCutRecord에 있다 (기록이 없으면 새 section, 슬롯 1)
        const auto capOf = [&](const MeshFace& face) -> const MeshCutRecord*
        {
            for (const MeshCutRecord& cap : piece.caps)
                if (cap.cutId == face.cutId)
                    return &cap;
            return nullptr;
        };
        const auto inLastSection = [&](const MeshFace& face)
        {
            const MeshCutRecord* cap = capOf(face);
            return cap != nullptr && cap->inLastSection;
        };
        const auto capSlot = [&](const MeshFace& face)
        {
            const MeshCutRecord* cap = capOf(face);
            return cap != nullptr ? cap->materialSlot : 1u;
        };
        const auto add = [&](const MeshFace& face)
        {
            out.indices.insert(out.indices.end(), { face.corner[0], face.corner[1], face.corner[2] });
        };

        // Surface passes: all (or never divided), then divided (only with splitByOrigin). The last
        // surface pass also takes the cap faces of UseLastSectionForCap cuts.
        const int lastSurfacePass = options.splitByOrigin ? 1 : 0;
        for (int pass = 0; pass <= lastSurfacePass; ++pass)
        {
            const uint32_t first = uint32_t(out.indices.size());
            for (const MeshFace& face : piece.faces)
                if (face.kind == MeshFaceKind::Surface && (!options.splitByOrigin || (face.cutId != 0) == (pass == 1)))
                    add(face);
            if (pass == lastSurfacePass)
                for (const MeshFace& face : piece.faces)
                    if (face.kind == MeshFaceKind::Cap && inLastSection(face))
                        add(face);
            const uint32_t count = uint32_t(out.indices.size()) - first;
            if (count > 0)
                out.sections.push_back({ first, count, 0u });
        }

        // Cap sections: one per material slot, in the order the slots first appear in the faces.
        std::vector<uint32_t> slots;
        for (const MeshFace& face : piece.faces)
        {
            if (face.kind != MeshFaceKind::Cap || inLastSection(face))
                continue;
            const uint32_t slot = capSlot(face);
            if (std::find(slots.begin(), slots.end(), slot) == slots.end())
                slots.push_back(slot);
        }
        for (uint32_t slot : slots)
        {
            const uint32_t first = uint32_t(out.indices.size());
            for (const MeshFace& face : piece.faces)
            {
                if (face.kind != MeshFaceKind::Cap || inLastSection(face))
                    continue;
                if (capSlot(face) == slot)
                    add(face);
            }
            out.sections.push_back({ first, uint32_t(out.indices.size()) - first, slot });
        }
    }

    bool UpdateRenderPositions(const MeshGeometry& piece, std::vector<MeshRenderVertex>& vertices)
    {
        if (vertices.size() != piece.corners.size())
            return false;
        for (size_t i = 0; i < vertices.size(); ++i)
        {
            if (piece.corners[i].vertex >= piece.vertices.size())
                return false;
        }
        for (size_t i = 0; i < vertices.size(); ++i)
            vertices[i].position = piece.vertices[piece.corners[i].vertex].position;
        return true;
    }

    // ---- Debug shapes: thin tubes for boundaries, loops and the cut plane (CutDebugView bakes them) ----

    DirectX::XMFLOAT4 EdgeTagColor(MeshEdgeTag kind)
    {
        switch (kind)
        {
        case MeshEdgeTag::Replacement: return { 1.0f, 0.95f, 0.0f, 1.0f };     // yellow
        case MeshEdgeTag::UnweldedSeam: return { 0.1f, 1.0f, 0.1f, 1.0f };    // green
        case MeshEdgeTag::Cut: return { 1.0f, 0.45f, 0.0f, 1.0f };            // orange
        case MeshEdgeTag::CapSeam: return { 1.0f, 1.0f, 1.0f, 1.0f };         // white
        case MeshEdgeTag::OpenSurface: return { 0.2f, 0.6f, 1.0f, 1.0f };     // blue
        case MeshEdgeTag::Interior: break;
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
                MeshRenderVertex vertex;
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

    void BakeBoundaryDebug(const MeshGeometry& piece, float radius, ProceduralMeshData& out)
    {
        out = ProceduralMeshData();
        std::vector<uint64_t> keys;
        keys.reserve(piece.edgeTags.size());
        for (const auto& entry : piece.edgeTags)
        {
            // CapSeam edges are interior; they are tagged but not drawn as open boundary.
            if (entry.second != MeshEdgeTag::CapSeam)
                keys.push_back(entry.first);
        }
        std::sort(keys.begin(), keys.end());
        for (uint64_t key : keys)
        {
            const uint32_t a = EdgeKeyLow(key), b = EdgeKeyHigh(key);
            if (a >= piece.vertices.size() || b >= piece.vertices.size())
                continue;
            AppendDebugSegment(out, piece.vertices[a].position, piece.vertices[b].position, radius,
                EdgeTagColor(piece.edgeTags.at(key)));
        }
        if (!out.indices.empty())
            out.sections.push_back({ 0, uint32_t(out.indices.size()), 0 });
    }

    void BakeLoopDebug(const MeshGeometry& piece, const std::vector<std::vector<uint32_t>>& loops, float radius,
        const DirectX::XMFLOAT4& color, ProceduralMeshData& out)
    {
        out = ProceduralMeshData();
        for (const std::vector<uint32_t>& loop : loops)
        {
            for (size_t i = 0; i < loop.size(); ++i)
            {
                const uint32_t a = loop[i], b = loop[(i + 1) % loop.size()];
                if (a < piece.vertices.size() && b < piece.vertices.size())
                    AppendDebugSegment(out, piece.vertices[a].position, piece.vertices[b].position, radius, color);
            }
        }
        if (!out.indices.empty())
            out.sections.push_back({ 0, uint32_t(out.indices.size()), 0 });
    }
}
