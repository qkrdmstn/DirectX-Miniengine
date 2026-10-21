#pragma once

// 이 파일은 패치 조각(PatchPiece)의 자료구조와, 렌더 정점을 지오메트리 노드로 합치는(용접) BuildPatchPiece를 담는다.

// Slice working data for a procedural patch. Nothing in Geometry/ depends on Model or Core,
// so the whole slicing pipeline builds and tests without a D3D12 device.

#include "SliceTypes.h"
#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MeshCutting
{
    // Epsilon is this fraction of the piece bounds radius.
    constexpr float kDefaultEpsilonScale = 1e-5f;

    class Skinning;

    // 스킨 정점 최대 3개의 무게중심 조합 (skinVertex는 Skinning의 스킨 정점 번호, 쓰는 칸이 앞에 오름차순, 안 쓰는 칸은 kNoNode와 0)
    struct PatchOrigin
    {
        uint32_t skinVertex[3] = { kNoNode, kNoNode, kNoNode };
        float bary[3] = { 0.0f, 0.0f, 0.0f };
    };

    enum class PatchNodeKind : uint8_t { Surface, Cap };

    // A geometric point. Every render corner that refers to it copies this position.
    struct PatchNode
    {
        PatchNodeKind kind = PatchNodeKind::Surface;
        PatchOrigin origin;               // Surface nodes only
        uint32_t cutId = 0;                 // Cut that created the node (0 = original patch)
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    };

    // A render vertex: a node plus its own attributes (UV seams and hard normals live here).
    struct PatchCorner
    {
        uint32_t node = 0;
        DirectX::XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
        DirectX::XMFLOAT2 uv = { 0.0f, 0.0f };
        DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    };

    enum class PatchFaceKind : uint8_t { Surface, Cap };

    struct PatchFace
    {
        uint32_t corner[3] = { 0, 0, 0 };
        PatchFaceKind kind = PatchFaceKind::Surface;
        uint32_t sourceTriangle = kNoNode;  // BonePatchSource::triangles index (kNoNode for Cap)
        uint32_t cutId = 0;                 // Cap: the cut that made it. Surface: the cut that divided it (0 = never divided)
    };

    // Kind of a tagged edge. Interior edges carry no entry in PatchPiece::boundary.
    enum class PatchEdgeKind : uint8_t
    {
        Interior,
        Replacement,    // open boundary of the extracted patch
        UnweldedSeam,   // seam that welding did not merge (skin or mesh slot differs)
        Cut,            // boundary created by a cut that has no cap yet
        CapSeam         // edge shared by a cap and the surface (same topology as Interior)
    };

    // One cut = one cap record, stored on both pieces.
    struct PatchCap
    {
        uint32_t cutId = 0;
        DirectX::XMFLOAT3 planePoint = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 planeNormal = { 0.0f, 1.0f, 0.0f };   // pointing out of the piece
        std::vector<uint32_t> rimNodes;    // all loops, outer loops first, each in loop order
        std::vector<uint32_t> loopSizes;    // node count of every loop in rimNodes
    };

    struct PatchPiece
    {
        std::shared_ptr<const Skinning> skin;   // 같은 패치에서 잘려 나온 piece들이 공유한다
        std::vector<PatchNode> nodes;
        std::vector<PatchCorner> corners;
        std::vector<PatchFace> faces;
        // Sorted (nodeA, nodeB) -> kind. Holds every tagged edge: Replacement, UnweldedSeam and
        // Cut are boundary edges, CapSeam edges are interior. Rebuilt for each piece after a cut.
        std::unordered_map<uint64_t, PatchEdgeKind> boundary;
        std::vector<PatchCap> caps;
        uint32_t skinStateId = 0;           // 0 = skinned exactly like the source patch

        bool Empty() const { return faces.empty(); }
        void Clear() { *this = PatchPiece(); }
    };

    const char* SliceStatusName(SliceStatus status);
    const char* EdgeKindName(PatchEdgeKind kind);

    struct SlicePlane
    {
        DirectX::XMFLOAT3 point = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
    };

    // Finite values and a normal longer than 1e-6.
    SliceStatus ValidatePlane(const SlicePlane& plane, std::string* detail = nullptr);

    // Right-handed frame of a plane: u x v = n. Loops, caps and debug drawing share it so a
    // loop's signed area has one meaning everywhere.
    struct SlicePlaneBasis
    {
        double origin[3] = { 0.0, 0.0, 0.0 };
        double n[3] = { 0.0, 1.0, 0.0 };    // unit normal
        double u[3] = { 1.0, 0.0, 0.0 };
        double v[3] = { 0.0, 0.0, -1.0 };
    };
    // Returns false when the plane fails ValidatePlane.
    bool MakePlaneBasis(const SlicePlane& plane, SlicePlaneBasis& basis);

    // ---- edges ----

    inline uint64_t MakeEdgeKey(uint32_t a, uint32_t b)
    {
        const uint32_t low = a < b ? a : b;
        const uint32_t high = a < b ? b : a;
        return (uint64_t(low) << 32) | high;
    }
    inline uint32_t EdgeKeyLow(uint64_t key) { return uint32_t(key >> 32); }
    inline uint32_t EdgeKeyHigh(uint64_t key) { return uint32_t(key & 0xFFFFFFFFu); }

    // How the faces use one undirected edge. "Forward" means low node -> high node.
    struct PatchEdgeUsage
    {
        uint32_t count = 0;
        uint32_t forward = 0;
        uint32_t backward = 0;
    };
    using PatchEdgeUsageMap = std::unordered_map<uint64_t, PatchEdgeUsage>;

    // Requires every corner and node index to be in range (ValidatePatchPiece checks that).
    // Faces that repeat a node contribute no edge for the repeated pair.
    void CollectEdgeUsage(const PatchPiece& piece, PatchEdgeUsageMap& usage);

    // ---- measures ----

    // Radius of the sphere around the node bounds center (matches ProceduralMeshBounds).
    double ComputePieceRadius(const PatchPiece& piece);
    double FaceArea(const PatchPiece& piece, const PatchFace& face);
    double TotalArea(const PatchPiece& piece, PatchFaceKind kind);

    // ---- input validation (design 3.1) ----

    // Checks node positions and origins, corner and face indices, degenerate faces and edge
    // manifoldness. epsilon = ComputePieceRadius * epsilonScale; a face is degenerate when its
    // area is at most epsilon^2. detail receives the first failure (face / edge / node numbers).
    SliceStatus ValidatePatchPiece(const PatchPiece& piece, float epsilonScale,
        std::string* detail = nullptr);

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
