#pragma once

// ProceduralMesh.h의 MeshGeometry를 생성·검증한다. BuildMeshGeometry는 렌더 정점을 기하 정점으로 합친다(용접).

// Slice working data for a procedural patch. Nothing in Geometry/ depends on Model or Core,
// so the whole slicing pipeline builds and tests without a D3D12 device.

#include "../ProceduralMesh.h"
#include <DirectXMath.h>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace MeshCutting
{
    const char* SliceStatusName(SliceStatus status);
    const char* EdgeTagName(MeshEdgeTag kind);

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

    // How the faces use one undirected edge. "Forward" means low vertex -> high vertex.
    struct MeshEdgeUsage
    {
        uint32_t count = 0;
        uint32_t forward = 0;
        uint32_t backward = 0;
    };
    using MeshEdgeUsageMap = std::unordered_map<uint64_t, MeshEdgeUsage>;

    // Requires every corner and vertex index to be in range (ValidateMeshGeometry checks that).
    // Faces that repeat a vertex contribute no edge for the repeated pair.
    void CollectEdgeUsage(const MeshGeometry& piece, MeshEdgeUsageMap& usage);

    // ---- measures ----

    // Radius of the sphere around the vertex bounds center (matches ProceduralMeshBounds).
    double ComputePieceRadius(const MeshGeometry& piece);
    double FaceArea(const MeshGeometry& piece, const MeshFace& face);
    double TotalArea(const MeshGeometry& piece, MeshFaceKind kind);

    // ---- input validation (design 3.1) ----

    // Checks vertex positions and origins, corner and face indices, degenerate faces and edge
    // manifoldness. epsilon = ComputePieceRadius * epsilonScale; a face is degenerate when its
    // area is at most epsilon^2. detail receives the first failure (face / edge / vertex numbers).
    SliceStatus ValidateMeshGeometry(const MeshGeometry& piece, float epsilonScale,
        std::string* detail = nullptr);

    // Welds render vertices into geometry vertices. The key is (mesh slot, bind position bits,
    // skin pairs sorted by joint); there is no epsilon welding. Boundary edges are tagged
    // Replacement, or UnweldedSeam when another boundary edge covers the same bind positions
    // (same slot) or the same posed positions (different slots).
    // 모든 정점의 가중치가 0이면 강체 입력(스킨 없는 메시)으로 받는다. 일부만 0이면 InvalidInput이다.
    //
    // stats is filled as far as the build got, even on failure. On failure out is empty.
    // in.bindings는 in.vertices와 같은 크기여야 한다. 원본 복사의 출력물을 재포장하지 않고 읽는다.
    SliceStatus BuildMeshGeometry(const ProceduralMeshData& in, float epsilonScale, MeshGeometry& out,
        WeldStats& stats, std::string* detail = nullptr);
}
