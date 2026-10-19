#include "../pch.h"
#include "PatchWeld.h"

#include <algorithm>
#include <cmath>
#include <cstring>

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
        void CanonicalSkin(const WeldInputVertex& vertex, SkinNode& skin)
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

        bool Finite(const DirectX::XMFLOAT3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        std::string Num(uint64_t value) { return std::to_string(value); }
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
        std::unordered_map<WeldKey, uint32_t, WeldKeyHash> nodeOf;
        std::unordered_map<PositionKey, uint32_t, PositionKeyHash> positionNodeCount;
        std::vector<uint32_t> nodeOfVertex(in.vertices.size());

        for (size_t i = 0; i < in.vertices.size(); ++i)
        {
            const WeldInputVertex& v = in.vertices[i];
            SkinNode skin;
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

            GeomNode geom;
            geom.kind = NodeKind::Surface;
            geom.origin.node[0] = node;
            geom.origin.bary[0] = 1.0f;
            geom.position = v.position;     // first render vertex wins; later ones copy it
            piece.nodes.push_back(geom);
            piece.originNodes.push_back(skin);
        }

        for (size_t i = 0; i < in.vertices.size(); ++i)
        {
            const WeldInputVertex& v = in.vertices[i];
            RenderCorner corner;
            corner.node = nodeOfVertex[i];
            corner.normal = v.normal;
            corner.uv = v.uv;
            corner.color = v.color;
            piece.corners.push_back(corner);
        }
        for (size_t i = 0; i < in.indices.size(); i += 3)
        {
            Face face;
            for (int k = 0; k < 3; ++k)
                face.corner[k] = in.indices[i + k];
            face.kind = FaceKind::Surface;
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
        EdgeUsageMap usage;
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
            const SkinNode& skin = piece.originNodes[n];
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
        std::unordered_map<uint64_t, EdgeKind> boundary;
        for (uint64_t key : boundaryKeys)
        {
            const uint32_t a = EdgeKeyLow(key), b = EdgeKeyHigh(key);
            const bool seam = bindPairCount[pairKey(nodeBindId[a], nodeBindId[b])] > 1
                || worldPairCount[pairKey(nodeWorldId[a], nodeWorldId[b])] > 1;
            boundary[key] = seam ? EdgeKind::UnweldedSeam : EdgeKind::Replacement;
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
