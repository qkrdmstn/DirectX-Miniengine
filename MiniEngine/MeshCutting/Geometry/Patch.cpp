// 이 파일은 패치 조각 자료구조의 검증과 보조 함수(PatchGeometry)와, 렌더 정점을 노드로 합치는 용접(PatchWeld)을 구현한다.
#include "../pch.h"
#include "Patch.h"
#include "Skinning.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace MeshCutting
{
    namespace
    {
        // Checks the barycentric origin of a surface node. why receives the reason.
        bool ValidateOrigin(const PatchOrigin& origin, size_t skinVertexCount, std::string& why)
        {
            double sum = 0.0;
            bool ended = false;
            for (int i = 0; i < 3; ++i)
            {
                if (origin.skinVertex[i] == kNoNode)
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
                if (origin.skinVertex[i] >= skinVertexCount)
                {
                    why = "skin vertex " + Num(origin.skinVertex[i]) + " out of range";
                    return false;
                }
                if (i > 0 && origin.skinVertex[i - 1] >= origin.skinVertex[i])
                {
                    why = "skin vertices are not strictly ascending";
                    return false;
                }
                if (!std::isfinite(origin.bary[i]) || origin.bary[i] < 0.0f || origin.bary[i] > 1.0f)
                {
                    why = "weight outside [0, 1]";
                    return false;
                }
                sum += origin.bary[i];
            }
            if (origin.skinVertex[0] == kNoNode)
            {
                why = "no skin vertex";
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

    const char* EdgeKindName(PatchEdgeKind kind)
    {
        switch (kind)
        {
        case PatchEdgeKind::Interior: return "Interior";
        case PatchEdgeKind::Replacement: return "Replacement";
        case PatchEdgeKind::UnweldedSeam: return "UnweldedSeam";
        case PatchEdgeKind::Cut: return "Cut";
        case PatchEdgeKind::CapSeam: return "CapSeam";
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

    bool MakePlaneBasis(const SlicePlane& plane, SlicePlaneBasis& basis)
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

    void CollectEdgeUsage(const PatchPiece& piece, PatchEdgeUsageMap& usage)
    {
        usage.clear();
        usage.reserve(piece.faces.size() * 2);
        for (const PatchFace& face : piece.faces)
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
                PatchEdgeUsage& use = usage[MakeEdgeKey(from, to)];
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
        for (const PatchNode& node : piece.nodes)
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
        for (const PatchNode& node : piece.nodes)
        {
            const double dx = node.position.x - center[0];
            const double dy = node.position.y - center[1];
            const double dz = node.position.z - center[2];
            radiusSquared = std::max(radiusSquared, dx * dx + dy * dy + dz * dz);
        }
        return std::sqrt(radiusSquared);
    }

    double FaceArea(const PatchPiece& piece, const PatchFace& face)
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

    double TotalArea(const PatchPiece& piece, PatchFaceKind kind)
    {
        double total = 0.0;
        for (const PatchFace& face : piece.faces)
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

        const size_t skinVertexCount = piece.skin ? piece.skin->GetVertices().size() : 0;     // skin이 없으면 빈 표로 본다
        for (size_t i = 0; i < piece.nodes.size(); ++i)
        {
            const PatchNode& node = piece.nodes[i];
            if (!Finite(node.position))
                return Fail(detail, SliceStatus::InvalidInput, "node " + Num(i) + " position is not finite");
            if (node.kind == PatchNodeKind::Surface)
            {
                std::string why;
                if (!ValidateOrigin(node.origin, skinVertexCount, why))
                    return Fail(detail, SliceStatus::InvalidOrigin, "node " + Num(i) + " origin: " + why);
            }
        }

        for (size_t i = 0; i < piece.corners.size(); ++i)
        {
            const PatchCorner& corner = piece.corners[i];
            if (corner.node >= piece.nodes.size())
                return Fail(detail, SliceStatus::InvalidInput, "corner " + Num(i) + " refers to node " + Num(corner.node)
                    + " beyond " + Num(piece.nodes.size()) + " nodes");
            if (!Finite(corner.normal) || !Finite(corner.uv) || !Finite(corner.color))
                return Fail(detail, SliceStatus::InvalidInput, "corner " + Num(i) + " has a non-finite attribute");
        }

        const double epsilon = ComputePieceRadius(piece) * double(epsilonScale);
        for (size_t f = 0; f < piece.faces.size(); ++f)
        {
            const PatchFace& face = piece.faces[f];
            for (int k = 0; k < 3; ++k)
                if (face.corner[k] >= piece.corners.size())
                    return Fail(detail, SliceStatus::InvalidInput, "face " + Num(f) + " refers to corner "
                        + Num(face.corner[k]) + " beyond " + Num(piece.corners.size()) + " corners");
            if (face.kind != PatchFaceKind::Surface && face.kind != PatchFaceKind::Cap)
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

        PatchEdgeUsageMap usage;
        CollectEdgeUsage(piece, usage);
        // Report the lowest edge key so the message does not depend on hash order.
        uint64_t worst = 0;
        bool found = false;
        std::string worstMessage;
        for (const auto& entry : usage)
        {
            const PatchEdgeUsage& use = entry.second;
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
            else if (tag->second == PatchEdgeKind::Interior || tag->second == PatchEdgeKind::CapSeam)
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

    namespace
    {
        // -0 and +0 are the same position.
        uint32_t Bits(float value)
        {
            if (value == 0.0f)
                value = 0.0f;
            uint32_t bits;
            std::memcpy(&bits, &value, sizeof(bits));
            return bits;
        }

        uint64_t Mix(uint64_t hash, uint32_t value)
        {
            hash ^= value;
            return hash * 1099511628211ull;
        }

        struct PositionKey
        {
            uint32_t slot;
            uint32_t bits[3];
            bool operator==(const PositionKey& other) const
            {
                return slot == other.slot && bits[0] == other.bits[0] && bits[1] == other.bits[1]
                    && bits[2] == other.bits[2];
            }
        };

        struct PositionKeyHash
        {
            size_t operator()(const PositionKey& key) const
            {
                uint64_t hash = 14695981039346656037ull;
                hash = Mix(hash, key.slot);
                for (int i = 0; i < 3; ++i)
                    hash = Mix(hash, key.bits[i]);
                return size_t(hash);
            }
        };

        struct WeldKey
        {
            PositionKey position;
            uint32_t jointCount;
            uint16_t joints[4];
            uint32_t weightBits[4];
            bool operator==(const WeldKey& other) const
            {
                if (!(position == other.position) || jointCount != other.jointCount)
                    return false;
                for (uint32_t i = 0; i < jointCount; ++i)
                    if (joints[i] != other.joints[i] || weightBits[i] != other.weightBits[i])
                        return false;
                return true;
            }
        };

        struct WeldKeyHash
        {
            size_t operator()(const WeldKey& key) const
            {
                uint64_t hash = PositionKeyHash()(key.position);
                hash = Mix(hash, key.jointCount);
                for (uint32_t i = 0; i < key.jointCount; ++i)
                {
                    hash = Mix(hash, key.joints[i]);
                    hash = Mix(hash, key.weightBits[i]);
                }
                return size_t(hash);
            }
        };

        // Positive weights only, equal joints merged, ascending joint order.
        void CanonicalSkin(const WeldInputVertex& vertex, SkinVertex& skin)
        {
            skin.jointCount = 0;
            for (int slot = 0; slot < 4; ++slot)
            {
                if (vertex.weights[slot] <= 0.0f)
                    continue;
                uint32_t at = 0;
                while (at < skin.jointCount && skin.joints[at] != vertex.joints[slot])
                    ++at;
                if (at == skin.jointCount)
                {
                    skin.joints[at] = vertex.joints[slot];
                    skin.weights[at] = 0.0f;
                    ++skin.jointCount;
                }
                skin.weights[at] += vertex.weights[slot];
            }
            for (uint32_t i = 1; i < skin.jointCount; ++i)
            {
                for (uint32_t j = i; j > 0 && skin.joints[j - 1] > skin.joints[j]; --j)
                {
                    std::swap(skin.joints[j - 1], skin.joints[j]);
                    std::swap(skin.weights[j - 1], skin.weights[j]);
                }
            }
        }
    }

    SliceStatus BuildPatchPiece(const WeldInput& in, float epsilonScale, PatchPiece& out,
        WeldStats& stats, std::string* detail)
    {
        out = PatchPiece();
        stats = WeldStats();
        if (detail)
            detail->clear();

        if (in.indices.empty() || in.indices.size() % 3 != 0)
            return Fail(detail, SliceStatus::InvalidInput, "index count must be a positive multiple of three");

        for (size_t i = 0; i < in.vertices.size(); ++i)
        {
            const WeldInputVertex& v = in.vertices[i];
            bool finite = Finite(v.bindPosition) && Finite(v.position) && Finite(v.normal)
                && std::isfinite(v.uv.x) && std::isfinite(v.uv.y) && std::isfinite(v.color.x)
                && std::isfinite(v.color.y) && std::isfinite(v.color.z) && std::isfinite(v.color.w);
            float weightSum = 0.0f;
            for (int k = 0; k < 4; ++k)
            {
                finite = finite && std::isfinite(v.weights[k]) && v.weights[k] >= 0.0f;
                weightSum += v.weights[k];
            }
            if (!finite)
                return Fail(detail, SliceStatus::InvalidInput, "vertex " + Num(i) + " has a non-finite value or a negative weight");
            if (!(weightSum > 0.0f))
                return Fail(detail, SliceStatus::InvalidInput, "vertex " + Num(i) + " has no positive skin weight");
        }
        for (size_t i = 0; i < in.indices.size(); ++i)
            if (in.indices[i] >= in.vertices.size())
                return Fail(detail, SliceStatus::InvalidInput, "index " + Num(i) + " refers beyond the vertex array");

        PatchPiece piece;
        std::vector<SkinVertex> skinVertices;
        std::unordered_map<WeldKey, uint32_t, WeldKeyHash> nodeOf;
        std::unordered_map<PositionKey, uint32_t, PositionKeyHash> positionNodeCount;
        std::vector<uint32_t> nodeOfVertex(in.vertices.size());

        for (size_t i = 0; i < in.vertices.size(); ++i)
        {
            const WeldInputVertex& v = in.vertices[i];
            SkinVertex skin;
            skin.meshSlot = v.meshSlot;
            skin.bindPosition = v.bindPosition;
            CanonicalSkin(v, skin);

            WeldKey key = {};
            key.position.slot = v.meshSlot;
            key.position.bits[0] = Bits(v.bindPosition.x);
            key.position.bits[1] = Bits(v.bindPosition.y);
            key.position.bits[2] = Bits(v.bindPosition.z);
            key.jointCount = skin.jointCount;
            for (uint32_t k = 0; k < skin.jointCount; ++k)
            {
                key.joints[k] = skin.joints[k];
                key.weightBits[k] = Bits(skin.weights[k]);
            }

            const auto found = nodeOf.find(key);
            if (found != nodeOf.end())
            {
                nodeOfVertex[i] = found->second;
                continue;
            }

            const uint32_t node = uint32_t(piece.nodes.size());
            nodeOf.emplace(key, node);
            ++positionNodeCount[key.position];
            nodeOfVertex[i] = node;

            PatchNode geom;
            geom.kind = PatchNodeKind::Surface;
            geom.origin.skinVertex[0] = node;
            geom.origin.bary[0] = 1.0f;
            geom.position = v.position;     // first render vertex wins; later ones copy it
            piece.nodes.push_back(geom);
            skinVertices.push_back(skin);
        }
        piece.skin = std::make_shared<const Skinning>(std::move(skinVertices));

        for (size_t i = 0; i < in.vertices.size(); ++i)
        {
            const WeldInputVertex& v = in.vertices[i];
            PatchCorner corner;
            corner.node = nodeOfVertex[i];
            corner.normal = v.normal;
            corner.uv = v.uv;
            corner.color = v.color;
            piece.corners.push_back(corner);
        }
        for (size_t i = 0; i < in.indices.size(); i += 3)
        {
            PatchFace face;
            for (int k = 0; k < 3; ++k)
                face.corner[k] = in.indices[i + k];
            face.kind = PatchFaceKind::Surface;
            face.sourceTriangle = uint32_t(i / 3);
            face.cutId = 0;
            piece.faces.push_back(face);
        }

        stats.corners = uint32_t(in.vertices.size());
        stats.nodes = uint32_t(piece.nodes.size());
        stats.positionNodes = uint32_t(positionNodeCount.size());
        for (const auto& entry : positionNodeCount)
            if (entry.second > 1)
                ++stats.skinSplitPositions;
        stats.faces = uint32_t(piece.faces.size());

        // Boundary edges and their kinds. Indices are valid by construction here.
        PatchEdgeUsageMap usage;
        CollectEdgeUsage(piece, usage);
        std::vector<uint64_t> boundaryKeys;
        for (const auto& entry : usage)
        {
            if (entry.second.count > 2 || (entry.second.count == 2 && (entry.second.forward != 1 || entry.second.backward != 1)))
                ++stats.nonManifoldEdges;
            else if (entry.second.count == 1)
                boundaryKeys.push_back(entry.first);
        }
        std::sort(boundaryKeys.begin(), boundaryKeys.end());

        // A boundary edge is an unwelded seam when another boundary edge covers the same bind
        // positions of the same slot, or the same posed positions (separate slots).
        std::unordered_map<PositionKey, uint32_t, PositionKeyHash> bindId, worldId;
        std::vector<uint32_t> nodeBindId(piece.nodes.size()), nodeWorldId(piece.nodes.size());
        for (size_t n = 0; n < piece.nodes.size(); ++n)
        {
            const SkinVertex& skin = piece.skin->GetVertices()[n];
            PositionKey bind = { skin.meshSlot, { Bits(skin.bindPosition.x), Bits(skin.bindPosition.y), Bits(skin.bindPosition.z) } };
            PositionKey world = { 0xFFFFFFFFu, { Bits(piece.nodes[n].position.x), Bits(piece.nodes[n].position.y), Bits(piece.nodes[n].position.z) } };
            nodeBindId[n] = bindId.emplace(bind, uint32_t(bindId.size())).first->second;
            nodeWorldId[n] = worldId.emplace(world, uint32_t(worldId.size())).first->second;
        }
        const auto pairKey = [](uint32_t a, uint32_t b) { return MakeEdgeKey(a, b); };
        std::unordered_map<uint64_t, uint32_t> bindPairCount, worldPairCount;
        for (uint64_t key : boundaryKeys)
        {
            const uint32_t a = EdgeKeyLow(key), b = EdgeKeyHigh(key);
            ++bindPairCount[pairKey(nodeBindId[a], nodeBindId[b])];
            ++worldPairCount[pairKey(nodeWorldId[a], nodeWorldId[b])];
        }
        std::unordered_map<uint64_t, PatchEdgeKind> boundary;
        for (uint64_t key : boundaryKeys)
        {
            const uint32_t a = EdgeKeyLow(key), b = EdgeKeyHigh(key);
            const bool seam = bindPairCount[pairKey(nodeBindId[a], nodeBindId[b])] > 1
                || worldPairCount[pairKey(nodeWorldId[a], nodeWorldId[b])] > 1;
            boundary[key] = seam ? PatchEdgeKind::UnweldedSeam : PatchEdgeKind::Replacement;
            ++stats.boundaryEdges;
            if (seam)
                ++stats.unweldedSeamEdges;
            else
                ++stats.replacementEdges;
        }

        piece.boundary = std::move(boundary);
        std::string why;
        const SliceStatus status = ValidatePatchPiece(piece, epsilonScale, &why);
        if (status != SliceStatus::Ok)
            return Fail(detail, status, why);

        out = std::move(piece);
        return SliceStatus::Ok;
    }
}
