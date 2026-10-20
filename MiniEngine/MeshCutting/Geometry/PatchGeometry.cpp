#include "../pch.h"
#include "PatchGeometry.h"

#include <algorithm>
#include <cmath>

namespace MeshCutting
{
    namespace
    {
        SliceStatus Fail(std::string* detail, SliceStatus status, const std::string& message)
        {
            if (detail)
                *detail = message;
            return status;
        }

        bool Finite(const DirectX::XMFLOAT2& v) { return std::isfinite(v.x) && std::isfinite(v.y); }
        bool Finite(const DirectX::XMFLOAT3& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }
        bool Finite(const DirectX::XMFLOAT4& v)
        {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
        }

        std::string Num(uint64_t value) { return std::to_string(value); }

        // Checks the barycentric origin of a surface node. why receives the reason.
        bool ValidateOrigin(const SurfaceOrigin& origin, size_t originCount, std::string& why)
        {
            double sum = 0.0;
            bool ended = false;
            for (int i = 0; i < 3; ++i)
            {
                if (origin.node[i] == kNoNode)
                {
                    if (origin.bary[i] != 0.0f)
                    {
                        why = "unused slot has a non-zero weight";
                        return false;
                    }
                    ended = true;
                    continue;
                }
                if (ended)
                {
                    why = "used slot after an unused slot";
                    return false;
                }
                if (origin.node[i] >= originCount)
                {
                    why = "origin node " + Num(origin.node[i]) + " out of range";
                    return false;
                }
                if (i > 0 && origin.node[i - 1] >= origin.node[i])
                {
                    why = "origin nodes are not strictly ascending";
                    return false;
                }
                if (!std::isfinite(origin.bary[i]) || origin.bary[i] < 0.0f || origin.bary[i] > 1.0f)
                {
                    why = "weight outside [0, 1]";
                    return false;
                }
                sum += origin.bary[i];
            }
            if (origin.node[0] == kNoNode)
            {
                why = "no origin node";
                return false;
            }
            if (std::fabs(sum - 1.0) > 1e-4)
            {
                why = "weights sum to " + std::to_string(sum);
                return false;
            }
            return true;
        }
    }

    const char* SliceStatusName(SliceStatus status)
    {
        switch (status)
        {
        case SliceStatus::Ok: return "Ok";
        case SliceStatus::NoCut: return "NoCut";
        case SliceStatus::InvalidInput: return "InvalidInput";
        case SliceStatus::InvalidOrigin: return "InvalidOrigin";
        case SliceStatus::NonManifoldInput: return "NonManifoldInput";
        case SliceStatus::DegenerateInput: return "DegenerateInput";
        case SliceStatus::CoplanarFace: return "CoplanarFace";
        case SliceStatus::CrossesCap: return "CrossesCap";
        case SliceStatus::OpenLoop: return "OpenLoop";
        case SliceStatus::NonManifoldLoop: return "NonManifoldLoop";
        case SliceStatus::InvalidLoop: return "InvalidLoop";
        case SliceStatus::CapTriangulationFailed: return "CapTriangulationFailed";
        case SliceStatus::LoopOutsidePlane: return "LoopOutsidePlane";
        }
        return "Unknown";
    }

    const char* EdgeKindName(EdgeKind kind)
    {
        switch (kind)
        {
        case EdgeKind::Interior: return "Interior";
        case EdgeKind::Replacement: return "Replacement";
        case EdgeKind::UnweldedSeam: return "UnweldedSeam";
        case EdgeKind::Cut: return "Cut";
        case EdgeKind::CapSeam: return "CapSeam";
        }
        return "Unknown";
    }

    SliceStatus ValidatePlane(const SlicePlane& plane, std::string* detail)
    {
        if (!Finite(plane.point) || !Finite(plane.normal))
            return Fail(detail, SliceStatus::InvalidInput, "plane has a non-finite value");
        const double nx = plane.normal.x, ny = plane.normal.y, nz = plane.normal.z;
        if (std::sqrt(nx * nx + ny * ny + nz * nz) <= 1e-6)
            return Fail(detail, SliceStatus::InvalidInput, "plane normal is too short");
        return SliceStatus::Ok;
    }

    bool MakePlaneBasis(const SlicePlane& plane, PlaneBasis& basis)
    {
        if (ValidatePlane(plane) != SliceStatus::Ok)
            return false;
        double n[3] = { plane.normal.x, plane.normal.y, plane.normal.z };
        const double length = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (double& c : n)
            c /= length;

        // The coordinate axis least aligned with n gives the most stable perpendicular.
        int least = 0;
        if (std::fabs(n[1]) < std::fabs(n[least]))
            least = 1;
        if (std::fabs(n[2]) < std::fabs(n[least]))
            least = 2;
        double axis[3] = { 0.0, 0.0, 0.0 };
        axis[least] = 1.0;
        double u[3] = { n[1] * axis[2] - n[2] * axis[1], n[2] * axis[0] - n[0] * axis[2], n[0] * axis[1] - n[1] * axis[0] };
        const double ulen = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
        for (double& c : u)
            c /= ulen;
        const double v[3] = { n[1] * u[2] - n[2] * u[1], n[2] * u[0] - n[0] * u[2], n[0] * u[1] - n[1] * u[0] };

        basis.origin[0] = plane.point.x; basis.origin[1] = plane.point.y; basis.origin[2] = plane.point.z;
        for (int k = 0; k < 3; ++k)
        {
            basis.n[k] = n[k];
            basis.u[k] = u[k];
            basis.v[k] = v[k];
        }
        return true;
    }

    void CollectEdgeUsage(const PatchPiece& piece, EdgeUsageMap& usage)
    {
        usage.clear();
        usage.reserve(piece.faces.size() * 2);
        for (const Face& face : piece.faces)
        {
            uint32_t node[3];
            for (int k = 0; k < 3; ++k)
                node[k] = piece.corners[face.corner[k]].node;
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t from = node[k];
                const uint32_t to = node[(k + 1) % 3];
                if (from == to)
                    continue;
                EdgeUsage& use = usage[MakeEdgeKey(from, to)];
                ++use.count;
                if (from < to)
                    ++use.forward;
                else
                    ++use.backward;
            }
        }
    }

    double ComputePieceRadius(const PatchPiece& piece)
    {
        if (piece.nodes.empty())
            return 0.0;
        double lo[3] = { piece.nodes[0].position.x, piece.nodes[0].position.y, piece.nodes[0].position.z };
        double hi[3] = { lo[0], lo[1], lo[2] };
        for (const GeomNode& node : piece.nodes)
        {
            const double p[3] = { node.position.x, node.position.y, node.position.z };
            for (int k = 0; k < 3; ++k)
            {
                lo[k] = std::min(lo[k], p[k]);
                hi[k] = std::max(hi[k], p[k]);
            }
        }
        const double center[3] = { (lo[0] + hi[0]) * 0.5, (lo[1] + hi[1]) * 0.5, (lo[2] + hi[2]) * 0.5 };
        double radiusSquared = 0.0;
        for (const GeomNode& node : piece.nodes)
        {
            const double dx = node.position.x - center[0];
            const double dy = node.position.y - center[1];
            const double dz = node.position.z - center[2];
            radiusSquared = std::max(radiusSquared, dx * dx + dy * dy + dz * dz);
        }
        return std::sqrt(radiusSquared);
    }

    double FaceArea(const PatchPiece& piece, const Face& face)
    {
        const DirectX::XMFLOAT3& a = piece.nodes[piece.corners[face.corner[0]].node].position;
        const DirectX::XMFLOAT3& b = piece.nodes[piece.corners[face.corner[1]].node].position;
        const DirectX::XMFLOAT3& c = piece.nodes[piece.corners[face.corner[2]].node].position;
        const double e1[3] = { double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z };
        const double e2[3] = { double(c.x) - a.x, double(c.y) - a.y, double(c.z) - a.z };
        const double cx = e1[1] * e2[2] - e1[2] * e2[1];
        const double cy = e1[2] * e2[0] - e1[0] * e2[2];
        const double cz = e1[0] * e2[1] - e1[1] * e2[0];
        return 0.5 * std::sqrt(cx * cx + cy * cy + cz * cz);
    }

    double TotalArea(const PatchPiece& piece, FaceKind kind)
    {
        double total = 0.0;
        for (const Face& face : piece.faces)
            if (face.kind == kind)
                total += FaceArea(piece, face);
        return total;
    }

    SliceStatus ValidatePatchPiece(const PatchPiece& piece, float epsilonScale, std::string* detail)
    {
        if (!std::isfinite(epsilonScale) || epsilonScale < 0.0f)
            return Fail(detail, SliceStatus::InvalidInput, "epsilon scale is not a finite non-negative value");
        if (piece.faces.empty())
            return Fail(detail, SliceStatus::InvalidInput, "piece has no faces");

        for (size_t i = 0; i < piece.nodes.size(); ++i)
        {
            const GeomNode& node = piece.nodes[i];
            if (!Finite(node.position))
                return Fail(detail, SliceStatus::InvalidInput, "node " + Num(i) + " position is not finite");
            if (node.kind == NodeKind::Surface)
            {
                std::string why;
                if (!ValidateOrigin(node.origin, piece.originNodes.size(), why))
                    return Fail(detail, SliceStatus::InvalidOrigin, "node " + Num(i) + " origin: " + why);
            }
        }

        for (size_t i = 0; i < piece.corners.size(); ++i)
        {
            const RenderCorner& corner = piece.corners[i];
            if (corner.node >= piece.nodes.size())
                return Fail(detail, SliceStatus::InvalidInput, "corner " + Num(i) + " refers to node " + Num(corner.node)
                    + " beyond " + Num(piece.nodes.size()) + " nodes");
            if (!Finite(corner.normal) || !Finite(corner.uv) || !Finite(corner.color))
                return Fail(detail, SliceStatus::InvalidInput, "corner " + Num(i) + " has a non-finite attribute");
        }

        const double epsilon = ComputePieceRadius(piece) * double(epsilonScale);
        for (size_t f = 0; f < piece.faces.size(); ++f)
        {
            const Face& face = piece.faces[f];
            for (int k = 0; k < 3; ++k)
                if (face.corner[k] >= piece.corners.size())
                    return Fail(detail, SliceStatus::InvalidInput, "face " + Num(f) + " refers to corner "
                        + Num(face.corner[k]) + " beyond " + Num(piece.corners.size()) + " corners");
            if (face.kind != FaceKind::Surface && face.kind != FaceKind::Cap)
                return Fail(detail, SliceStatus::InvalidInput, "face " + Num(f) + " has an unknown kind");

            const uint32_t n0 = piece.corners[face.corner[0]].node;
            const uint32_t n1 = piece.corners[face.corner[1]].node;
            const uint32_t n2 = piece.corners[face.corner[2]].node;
            if (n0 == n1 || n1 == n2 || n0 == n2)
                return Fail(detail, SliceStatus::DegenerateInput, "face " + Num(f) + " repeats a node");
            if (FaceArea(piece, face) <= epsilon * epsilon)
                return Fail(detail, SliceStatus::DegenerateInput, "face " + Num(f) + " has area "
                    + std::to_string(FaceArea(piece, face)) + " <= epsilon^2");
        }

        EdgeUsageMap usage;
        CollectEdgeUsage(piece, usage);
        // Report the lowest edge key so the message does not depend on hash order.
        uint64_t worst = 0;
        bool found = false;
        std::string worstMessage;
        for (const auto& entry : usage)
        {
            const EdgeUsage& use = entry.second;
            std::string message;
            if (use.count > 2)
                message = "used by " + Num(use.count) + " faces";
            else if (use.count == 2 && (use.forward != 1 || use.backward != 1))
                message = "used in the same direction by two faces";
            else
                continue;
            if (!found || entry.first < worst)
            {
                found = true;
                worst = entry.first;
                worstMessage = message;
            }
        }
        if (found)
            return Fail(detail, SliceStatus::NonManifoldInput, "edge (" + Num(EdgeKeyLow(worst)) + ", "
                + Num(EdgeKeyHigh(worst)) + ") " + worstMessage);

        // Every open edge must say what it is; an untagged boundary cannot be told from a cut.
        found = false;
        for (const auto& entry : usage)
        {
            if (entry.second.count != 1)
                continue;
            const auto tag = piece.boundary.find(entry.first);
            std::string message;
            if (tag == piece.boundary.end())
                message = "boundary edge has no kind";
            else if (tag->second == EdgeKind::Interior || tag->second == EdgeKind::CapSeam)
                message = std::string("boundary edge is tagged ") + EdgeKindName(tag->second);
            else
                continue;
            if (!found || entry.first < worst)
            {
                found = true;
                worst = entry.first;
                worstMessage = message;
            }
        }
        if (found)
            return Fail(detail, SliceStatus::InvalidInput, "edge (" + Num(EdgeKeyLow(worst)) + ", "
                + Num(EdgeKeyHigh(worst)) + "): " + worstMessage);

        if (detail)
            detail->clear();
        return SliceStatus::Ok;
    }
}
