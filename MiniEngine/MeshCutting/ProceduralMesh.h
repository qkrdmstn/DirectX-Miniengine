#pragma once

#include <DirectXMath.h>
#include <cstdint>
#include <string>
#include <vector>

namespace MeshCutting
{
    struct ProceduralVertex
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

    struct ProceduralMeshData
    {
        std::vector<ProceduralVertex> vertices;
        std::vector<uint32_t> indices;
        std::vector<MeshSection> sections;
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
        bool SetMesh(ProceduralMeshData data, std::string* error = nullptr);
        bool UpdateVertices(const std::vector<ProceduralVertex>& vertices, std::string* error = nullptr);
        void Clear();

        const ProceduralMeshData& GetData() const { return m_Data; }
        const ProceduralMeshBounds& GetBounds() const { return m_Bounds; }
        uint64_t GetVertexVersion() const { return m_VertexVersion; }
        uint64_t GetTopologyVersion() const { return m_TopologyVersion; }

    private:
        ProceduralMeshData m_Data;
        ProceduralMeshBounds m_Bounds;
        uint64_t m_VertexVersion = 0;
        uint64_t m_TopologyVersion = 0;
    };
}
