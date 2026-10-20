#pragma once

// Slice working data for a procedural patch. Nothing in Geometry/ depends on Model or Core,
// so the whole slicing pipeline builds and tests without a D3D12 device.

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace MeshCutting
{
    constexpr uint32_t kNoNode = 0xFFFFFFFFu;

    // Epsilon is this fraction of the piece bounds radius.
    constexpr float kDefaultEpsilonScale = 1e-5f;

    // Skin data of one original patch node. Surface nodes created by slicing refer to these
    // through SurfaceOrigin, so every piece can be skinned from the same source later.
    struct SkinNode
    {
        uint32_t meshSlot = 0;
        DirectX::XMFLOAT3 bindPosition = { 0.0f, 0.0f, 0.0f };
        uint32_t jointCount = 0;            // joints[0..jointCount), ascending, weights > 0
        uint16_t joints[4] = {};
        float weights[4] = {};
    };

    // Barycentric origin of a surface node over at most three original nodes (SkinNode indices).
    // Used slots come first with ascending node index; unused slots hold kNoNode and weight 0.
    struct SurfaceOrigin
    {
        uint32_t node[3] = { kNoNode, kNoNode, kNoNode };
        float bary[3] = { 0.0f, 0.0f, 0.0f };
    };

    enum class NodeKind : uint8_t { Surface, Cap };

    // A geometric point. Every render corner that refers to it copies this position.
    struct GeomNode
    {
        NodeKind kind = NodeKind::Surface;
        SurfaceOrigin origin;               // Surface nodes only
        uint32_t cutId = 0;                 // Cut that created the node (0 = original patch)
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    };

    // A render vertex: a node plus its own attributes (UV seams and hard normals live here).
    struct RenderCorner
    {
        uint32_t node = 0;
        DirectX::XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
        DirectX::XMFLOAT2 uv = { 0.0f, 0.0f };
        DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    };

    enum class FaceKind : uint8_t { Surface, Cap };

    struct Face
    {
        uint32_t corner[3] = { 0, 0, 0 };
        FaceKind kind = FaceKind::Surface;
        uint32_t sourceTriangle = kNoNode;  // BonePatchSource::triangles index (kNoNode for Cap)
        uint32_t cutId = 0;                 // Cap: the cut that made it. Surface: the cut that divided it (0 = never divided)
    };

    // Kind of a tagged edge. Interior edges carry no entry in PatchPiece::boundary.
    enum class EdgeKind : uint8_t
    {
        Interior,
        Replacement,    // open boundary of the extracted patch
        UnweldedSeam,   // seam that welding did not merge (skin or mesh slot differs)
        Cut,            // boundary created by a cut that has no cap yet
        CapSeam         // edge shared by a cap and the surface (same topology as Interior)
    };

    // One cut = one cap record, stored on both pieces.
    struct CapRecord
    {
        uint32_t cutId = 0;
        DirectX::XMFLOAT3 planePoint = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 planeNormal = { 0.0f, 1.0f, 0.0f };   // pointing out of the piece
        uint32_t attachJoint = kNoNode;                         // RigidToJoint data, stored only
        DirectX::XMFLOAT4X4 attachJointWorldAtCut = {
            1.0f, 0.0f, 0.0f, 0.0f,
            0.0f, 1.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 1.0f, 0.0f,
            0.0f, 0.0f, 0.0f, 1.0f };
        std::vector<uint32_t> rimNodes;     // all loops, outer loops first, each in loop order
        std::vector<uint32_t> loopSizes;    // node count of every loop in rimNodes
    };

    struct PatchPiece
    {
        std::vector<SkinNode> originNodes;  // shared by all pieces cut from one patch
        std::vector<GeomNode> nodes;
        std::vector<RenderCorner> corners;
        std::vector<Face> faces;
        // Sorted (nodeA, nodeB) -> kind. Holds every tagged edge: Replacement, UnweldedSeam and
        // Cut are boundary edges, CapSeam edges are interior. Rebuilt for each piece after a cut.
        std::unordered_map<uint64_t, EdgeKind> boundary;
        std::vector<CapRecord> caps;
        uint32_t skinStateId = 0;           // 0 = skinned exactly like the source patch

        bool Empty() const { return faces.empty(); }
        void Clear() { *this = PatchPiece(); }
    };

    enum class SliceStatus
    {
        Ok,                     // both pieces are non-empty; loops may be empty
        NoCut,                  // every face lies on one side (touching included)
        InvalidInput,           // non-finite value, bad plane, index out of range
        InvalidOrigin,          // bary out of range or sum != 1, more than three origin nodes
        NonManifoldInput,       // edge used by 3+ faces, or two faces use it the same way
        DegenerateInput,        // zero-area face or repeated node in a face
        CoplanarFace,           // a face lies in the plane
        CrossesCap,             // the plane cuts an existing cap face
        OpenLoop,               // cut loop runs into a Replacement/UnweldedSeam boundary
        NonManifoldLoop,        // cut edges do not form simple loops
        InvalidLoop,            // self/mutual crossing, unsupported nesting, wrong winding
        CapTriangulationFailed,
        LoopOutsidePlane        // set by the caller (CutController), never by SlicePatch: a cut loop leaves the finite CutPlane
    };

    const char* SliceStatusName(SliceStatus status);
    const char* EdgeKindName(EdgeKind kind);

    struct SlicePlane
    {
        DirectX::XMFLOAT3 point = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
    };

    // Finite values and a normal longer than 1e-6.
    SliceStatus ValidatePlane(const SlicePlane& plane, std::string* detail = nullptr);

    // Right-handed frame of a plane: u x v = n. Loops, caps and debug drawing share it so a
    // loop's signed area has one meaning everywhere.
    struct PlaneBasis
    {
        double origin[3] = { 0.0, 0.0, 0.0 };
        double n[3] = { 0.0, 1.0, 0.0 };    // unit normal
        double u[3] = { 1.0, 0.0, 0.0 };
        double v[3] = { 0.0, 0.0, -1.0 };
    };
    // Returns false when the plane fails ValidatePlane.
    bool MakePlaneBasis(const SlicePlane& plane, PlaneBasis& basis);

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
    struct EdgeUsage
    {
        uint32_t count = 0;
        uint32_t forward = 0;
        uint32_t backward = 0;
    };
    using EdgeUsageMap = std::unordered_map<uint64_t, EdgeUsage>;

    // Requires every corner and node index to be in range (ValidatePatchPiece checks that).
    // Faces that repeat a node contribute no edge for the repeated pair.
    void CollectEdgeUsage(const PatchPiece& piece, EdgeUsageMap& usage);

    // ---- measures ----

    // Radius of the sphere around the node bounds center (matches ProceduralMeshBounds).
    double ComputePieceRadius(const PatchPiece& piece);
    double FaceArea(const PatchPiece& piece, const Face& face);
    double TotalArea(const PatchPiece& piece, FaceKind kind);

    // ---- input validation (design 3.1) ----

    // Checks node positions and origins, corner and face indices, degenerate faces and edge
    // manifoldness. epsilon = ComputePieceRadius * epsilonScale; a face is degenerate when its
    // area is at most epsilon^2. detail receives the first failure (face / edge / node numbers).
    SliceStatus ValidatePatchPiece(const PatchPiece& piece, float epsilonScale,
        std::string* detail = nullptr);
}
