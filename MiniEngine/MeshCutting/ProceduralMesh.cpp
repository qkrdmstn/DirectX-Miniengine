#include "pch.h"
#include "ProceduralMesh.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

namespace MeshCutting
{
    namespace
    {
        // Versions are unique across all meshes, so a renderer entry left behind by a destroyed
        // mesh can never match a different mesh that later reuses the same address.
        uint64_t NextVersion()
        {
            static std::atomic<uint64_t> s_Version{ 0 };
            return ++s_Version;
        }

        bool Fail(std::string* error, const char* message)
        {
            if (error)
                *error = message;
            return false;
        }

        bool Finite(const DirectX::XMFLOAT3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool Finite(const DirectX::XMFLOAT2& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y);
        }

        bool Finite(const DirectX::XMFLOAT4& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z) && std::isfinite(value.w);
        }

        bool ValidateVertices(const std::vector<ProceduralVertex>& vertices,
            ProceduralMeshBounds& bounds, std::string* error)
        {
            if (vertices.size() > std::numeric_limits<uint32_t>::max() ||
                vertices.size() > std::numeric_limits<uint32_t>::max() / sizeof(ProceduralVertex))
                return Fail(error, "Vertex buffer exceeds D3D12 byte-size limit");

            if (vertices.empty())
            {
                bounds = {};
                return true;
            }

            for (const ProceduralVertex& vertex : vertices)
            {
                if (!Finite(vertex.position) || !Finite(vertex.normal) ||
                    !Finite(vertex.uv) || !Finite(vertex.color))
                    return Fail(error, "Vertex contains a non-finite attribute");
            }

            bounds.valid = true;
            bounds.min = vertices[0].position;
            bounds.max = vertices[0].position;
            for (const ProceduralVertex& vertex : vertices)
            {
                bounds.min.x = std::min(bounds.min.x, vertex.position.x);
                bounds.min.y = std::min(bounds.min.y, vertex.position.y);
                bounds.min.z = std::min(bounds.min.z, vertex.position.z);
                bounds.max.x = std::max(bounds.max.x, vertex.position.x);
                bounds.max.y = std::max(bounds.max.y, vertex.position.y);
                bounds.max.z = std::max(bounds.max.z, vertex.position.z);
            }

            const double cx = (double(bounds.min.x) + bounds.max.x) * 0.5;
            const double cy = (double(bounds.min.y) + bounds.max.y) * 0.5;
            const double cz = (double(bounds.min.z) + bounds.max.z) * 0.5;
            bounds.center = { float(cx), float(cy), float(cz) };
            double radiusSquared = 0.0;
            for (const ProceduralVertex& vertex : vertices)
            {
                const double dx = double(vertex.position.x) - bounds.center.x;
                const double dy = double(vertex.position.y) - bounds.center.y;
                const double dz = double(vertex.position.z) - bounds.center.z;
                radiusSquared = std::max(radiusSquared, dx * dx + dy * dy + dz * dz);
            }
            const double radius = std::sqrt(radiusSquared);
            if (radius > std::numeric_limits<float>::max())
                return Fail(error, "Mesh bounds exceed finite float range");
            bounds.radius = float(radius);
            if (double(bounds.radius) < radius)
                bounds.radius = std::nextafter(bounds.radius, std::numeric_limits<float>::infinity());
            return true;
        }

        bool ValidateTopology(ProceduralMeshData& data, std::string* error)
        {
            if (data.indices.size() > std::numeric_limits<uint32_t>::max() ||
                data.indices.size() > std::numeric_limits<uint32_t>::max() / sizeof(uint32_t))
                return Fail(error, "Index buffer exceeds D3D12 byte-size limit");
            if (data.indices.size() % 3 != 0)
                return Fail(error, "Index count must be a multiple of three");
            for (uint32_t index : data.indices)
            {
                if (index >= data.vertices.size())
                    return Fail(error, "Index refers beyond the vertex array");
            }

            // Omitted sections mean one material-slot-zero draw over all triangles.
            if (!data.indices.empty() && data.sections.empty())
                data.sections.push_back({ 0, uint32_t(data.indices.size()), 0 });
            if (data.indices.empty() && !data.sections.empty())
                return Fail(error, "Empty index data cannot have sections");

            std::vector<MeshSection> sorted = data.sections;
            std::sort(sorted.begin(), sorted.end(), [](const MeshSection& a, const MeshSection& b)
                { return a.firstIndex < b.firstIndex; });
            uint32_t previousEnd = 0;
            for (const MeshSection& section : sorted)
            {
                if (section.indexCount == 0 || section.firstIndex % 3 != 0 || section.indexCount % 3 != 0)
                    return Fail(error, "Section must contain whole triangles");
                if (section.firstIndex > data.indices.size() ||
                    section.indexCount > data.indices.size() - section.firstIndex)
                    return Fail(error, "Section extends beyond index data");
                if (section.firstIndex < previousEnd)
                    return Fail(error, "Sections overlap");
                previousEnd = section.firstIndex + section.indexCount;
            }
            return true;
        }
    }

    bool ProceduralMesh::SetMesh(ProceduralMeshData data, std::string* error)
    {
        ProceduralMeshBounds bounds;
        if (!ValidateVertices(data.vertices, bounds, error) || !ValidateTopology(data, error))
            return false;

        std::swap(m_Data, data);
        m_Bounds = bounds;
        m_VertexVersion = NextVersion();
        m_TopologyVersion = NextVersion();
        if (error)
            error->clear();
        return true;
    }

    bool ProceduralMesh::UpdateVertices(const std::vector<ProceduralVertex>& vertices, std::string* error)
    {
        if (vertices.size() != m_Data.vertices.size())
            return Fail(error, "UpdateVertices requires the existing vertex count");
        ProceduralMeshBounds bounds;
        if (!ValidateVertices(vertices, bounds, error))
            return false;

        std::vector<ProceduralVertex> replacement = vertices;
        m_Data.vertices.swap(replacement);
        m_Bounds = bounds;
        m_VertexVersion = NextVersion();
        if (error)
            error->clear();
        return true;
    }

    void ProceduralMesh::Clear()
    {
        ProceduralMeshData empty;
        std::swap(m_Data, empty);
        m_Bounds = {};
        m_VertexVersion = NextVersion();
        m_TopologyVersion = NextVersion();
    }
}
