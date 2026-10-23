#pragma once

#include "Geometry/SliceTypes.h"
#include <memory>
#include <unordered_map>
#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace MeshCutting
{
    // Epsilon is this fraction of the piece bounds radius.
    constexpr float kDefaultEpsilonScale = 1e-5f;

    class Skinning;

    // 스킨 정점 최대 3개의 무게중심 조합 (skinVertex는 Skinning의 스킨 정점 번호, 쓰는 칸이 앞에 오름차순, 안 쓰는 칸은 kInvalidIndex와 0)
    struct MeshVertexOrigin
    {
        uint32_t skinVertex[3] = { kInvalidIndex, kInvalidIndex, kInvalidIndex };
        float bary[3] = { 0.0f, 0.0f, 0.0f };
    };

    enum class MeshVertexKind : uint8_t { Surface, Cap };

    // A geometric point. Every render corner that refers to it copies this position.
    struct MeshVertex
    {
        MeshVertexKind kind = MeshVertexKind::Surface;
        MeshVertexOrigin origin;               // Surface vertices only
        uint32_t cutId = 0;                 // Cut that created the vertex (0 = original patch)
        DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f };
    };

    // A render vertex: a vertex plus its own attributes (UV seams and hard normals live here).
    struct MeshCorner
    {
        uint32_t vertex = 0;
        DirectX::XMFLOAT3 normal = { 0.0f, 1.0f, 0.0f };
        DirectX::XMFLOAT2 uv = { 0.0f, 0.0f };
        DirectX::XMFLOAT4 color = { 1.0f, 1.0f, 1.0f, 1.0f };
    };

    enum class MeshFaceKind : uint8_t { Surface, Cap };

    struct MeshFace
    {
        uint32_t corner[3] = { 0, 0, 0 };
        MeshFaceKind kind = MeshFaceKind::Surface;
        uint32_t sourceTriangle = kInvalidIndex;  // MeshSource::triangles index (kInvalidIndex for Cap)
        uint32_t cutId = 0;                 // Cap: the cut that made it. Surface: the cut that divided it (0 = never divided)
    };

    // Kind of a tagged edge. Interior edges carry no entry in MeshGeometry::edgeTags.
    enum class MeshEdgeTag : uint8_t
    {
        Interior,
        Replacement,    // open boundary of the extracted patch
        UnweldedSeam,   // seam that welding did not merge (skin or mesh slot differs)
        Cut,            // boundary created by a cut that has no cap yet
        CapSeam,        // edge shared by a cap and the surface (same topology as Interior)
        OpenSurface     // 원본 표면 자체의 구멍(스태틱 복사의 열린 경계). 절단선이 닿으면 그 사슬만 cap 없이 자른다 (Unreal 방식)
    };

    // One cut = one cap record, stored on both pieces.
    struct MeshCutRecord
    {
        uint32_t cutId = 0;
        DirectX::XMFLOAT3 planePoint = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 planeNormal = { 0.0f, 1.0f, 0.0f };   // pointing out of the piece
        std::vector<uint32_t> rimVertices;    // all loops, outer loops first, each in loop order
        std::vector<uint32_t> loopSizes;    // vertex count of every loop in rimVertices
        uint32_t materialSlot = 1;          // 렌더 section의 재질 번호 (cap 전용 section일 때)
        bool inLastSection = false;         // 참이면 cap 면을 마지막 표면 section에 합쳐 굽는다 (UseLastSectionForCap)
    };

    // Canonical storage owned by ProceduralMesh, also used by internal geometry algorithms.
    struct MeshGeometry
    {
        std::shared_ptr<const Skinning> skin;   // 같은 패치에서 잘려 나온 piece들이 공유한다
        std::vector<MeshVertex> vertices;
        std::vector<MeshCorner> corners;
        std::vector<MeshFace> faces;
        // Sorted (vertexA, vertexB) -> kind. Holds every tagged edge: Replacement, UnweldedSeam and
        // Cut are boundary edges, CapSeam edges are interior. Rebuilt for each piece after a cut.
        std::unordered_map<uint64_t, MeshEdgeTag> edgeTags;
        std::vector<MeshCutRecord> caps;
        uint32_t skinStateId = 0;           // 0 = skinned exactly like the source patch

        bool Empty() const { return faces.empty(); }
        void Clear() { *this = MeshGeometry(); }
    };

    struct WeldStats
    {
        uint32_t corners = 0;               // render vertices before welding
        uint32_t vertices = 0;                 // geometry vertices after welding
        uint32_t positionVertices = 0;         // vertices if skin equality were ignored (comparison only)
        uint32_t skinSplitPositions = 0;    // bind positions that ended up with more than one vertex
        uint32_t faces = 0;
        uint32_t boundaryEdges = 0;
        uint32_t replacementEdges = 0;
        uint32_t openSurfaceEdges = 0;      // 강체 입력의 열린 경계 (Replacement 대신)
        uint32_t unweldedSeamEdges = 0;     // counts both sides of a seam
        uint32_t nonManifoldEdges = 0;
    };

    struct MeshRenderVertex
    {
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        DirectX::XMFLOAT2 uv;
        DirectX::XMFLOAT4 color;
    };

    struct MeshSection
    {
        uint32_t firstIndex;
        uint32_t indexCount;
        uint32_t materialSlot;
    };

    // CPU-only deformation data, in render vertex order. Not part of the GPU vertex layout.
    struct MeshVertexBinding
    {
        uint32_t meshSlot = 0;
        DirectX::XMFLOAT3 bindPosition = { 0.0f, 0.0f, 0.0f };
        uint16_t joints[4] = {};
        float weights[4] = {};       // All zero: rigid vertex transformed by the mesh world matrix.
    };

    struct ProceduralMeshData
    {
        std::vector<MeshRenderVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<MeshSection> sections;
        std::vector<MeshVertexBinding> bindings;   // One per vertex for cutting; empty for render-only meshes.
    };

    struct ProceduralMeshBounds
    {
        DirectX::XMFLOAT3 min = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 max = { 0.0f, 0.0f, 0.0f };
        DirectX::XMFLOAT3 center = { 0.0f, 0.0f, 0.0f };
        float radius = 0.0f;
        bool valid = false;
    };

    // CPU data belongs to this object. A renderer that registers it must release
    // its registration before this object's lifetime ends.
    class ProceduralMesh
    {
    public:
        SliceStatus Build(const ProceduralMeshData& data, WeldStats& stats, std::string* error = nullptr);
        bool SetMesh(ProceduralMeshData data, std::string* error = nullptr);
        bool UpdateVertices(const std::vector<MeshRenderVertex>& vertices, std::string* error = nullptr);
        // Own cutting geometry and its render cache. Render-only meshes still use SetMesh.
        bool SetTopology(MeshGeometry geometry, std::string* error = nullptr);
        bool UpdatePose(const std::vector<std::vector<DirectX::XMFLOAT4X4>>& matrices, std::string* error = nullptr);
        bool SetSectionSplit(bool splitByOrigin, std::string* error = nullptr);
        void Clear();

        const MeshGeometry& GetGeometry() const { return m_Geometry; }
        const std::vector<MeshVertex>& GetVertices() const { return m_Geometry.vertices; }
        const std::vector<MeshCorner>& GetCorners() const { return m_Geometry.corners; }
        const std::vector<MeshFace>& GetFaces() const { return m_Geometry.faces; }
        const ProceduralMeshData& GetData() const { return m_Data; }
        const ProceduralMeshBounds& GetBounds() const { return m_Bounds; }
        uint64_t GetVertexVersion() const { return m_VertexVersion; }
        uint64_t GetTopologyVersion() const { return m_TopologyVersion; }

    private:
        bool SetRenderData(ProceduralMeshData data, std::string* error);
        MeshGeometry m_Geometry;
        bool m_SectionSplit = false;
        ProceduralMeshData m_Data;
        ProceduralMeshBounds m_Bounds;
        uint64_t m_VertexVersion = 0;
        uint64_t m_TopologyVersion = 0;
    };
}
