#include "pch.h"
#include "ProceduralMesh.h"
#include "Geometry/Skinning.h"
#include "Rendering/RenderBake.h"

#include <algorithm>
#include <atomic>
#include <cassert>
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

        bool IsFinite(const DirectX::XMFLOAT3& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
        }

        bool IsFinite(const DirectX::XMFLOAT2& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y);
        }

        bool IsFinite(const DirectX::XMFLOAT4& value)
        {
            return std::isfinite(value.x) && std::isfinite(value.y) &&
                std::isfinite(value.z) && std::isfinite(value.w);
        }

        // 모든 정점을 순회하며 유효성 검사를 수행하는 함수
        bool ValidateVertices(const std::vector<MeshRenderVertex>& vertices,
            ProceduralMeshBounds& bounds, std::string* error)
        {
            if (vertices.size() > std::numeric_limits<uint32_t>::max() ||
                vertices.size() > std::numeric_limits<uint32_t>::max() / sizeof(MeshRenderVertex))
                return Fail(error, "Vertex buffer exceeds D3D12 byte-size limit");

            if (vertices.empty())
            {
                bounds = {};
                return true;
            }

            
            // 정점 위치 유효성 검사
            for (const MeshRenderVertex& vertex : vertices)
            {
                if (!IsFinite(vertex.position) || !IsFinite(vertex.normal) ||
                    !IsFinite(vertex.uv) || !IsFinite(vertex.color))
                    return Fail(error, "Vertex contains a non-finite attribute");
            }

            // bounds 설정
            bounds.valid = true;
            bounds.min = vertices[0].position;
            bounds.max = vertices[0].position;
            for (const MeshRenderVertex& vertex : vertices)
            {
                bounds.min.x = std::min(bounds.min.x, vertex.position.x);
                bounds.min.y = std::min(bounds.min.y, vertex.position.y);
                bounds.min.z = std::min(bounds.min.z, vertex.position.z);
                bounds.max.x = std::max(bounds.max.x, vertex.position.x);
                bounds.max.y = std::max(bounds.max.y, vertex.position.y);
                bounds.max.z = std::max(bounds.max.z, vertex.position.z);
            }

            // 중심점 계산 및 Radius 설정
            const double cx = (double(bounds.min.x) + bounds.max.x) * 0.5;
            const double cy = (double(bounds.min.y) + bounds.max.y) * 0.5;
            const double cz = (double(bounds.min.z) + bounds.max.z) * 0.5;
            bounds.center = { float(cx), float(cy), float(cz) };
            double radiusSquared = 0.0;
            for (const MeshRenderVertex& vertex : vertices)
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

        // 삼각형 구성 검사
        bool ValidateTopology(ProceduralMeshData& data, std::string* error)
        {
            if (data.indices.size() > std::numeric_limits<uint32_t>::max() ||
                data.indices.size() > std::numeric_limits<uint32_t>::max() / sizeof(uint32_t))
                return Fail(error, "Index buffer exceeds D3D12 byte-size limit");

            // 삼각형을 담기 때문에, 3의 배수여야 한다.
            if (data.indices.size() % 3 != 0)
                return Fail(error, "Index count must be a multiple of three");
            for (uint32_t index : data.indices)
            {
                if (index >= data.vertices.size())
                    return Fail(error, "Index refers beyond the vertex array");
            }

            // section이 없는 경우, 모든 face를 0번 material로 렌더링한다.
            if (!data.indices.empty() && data.sections.empty())
                data.sections.push_back({ 0, uint32_t(data.indices.size()), 0 });

            // face가 비어있는데 section이 비어있지 않으면 실패
            if (data.indices.empty() && !data.sections.empty())
                return Fail(error, "Empty index data cannot have sections");

            // face가 두 section에 중복 포함돼있는지 검사한다.
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

    SliceStatus ProceduralMesh::Build(const ProceduralMeshData& data, WeldStats& stats, std::string* error)
    {
        MeshGeometry geometry;
        const SliceStatus status = BuildMeshGeometry(data, kDefaultEpsilonScale, geometry, stats, error);
        if (status != SliceStatus::Ok)
            return status;
        // BuildMeshGeometry가 이미 검증했으므로 다시 검증하지 않는다
        return AdoptValidatedTopology(std::move(geometry), error) ? SliceStatus::Ok : SliceStatus::InvalidInput;
    }

    // MSVC의 unordered_map(edgeTags) 이동 생성은 버킷을 할당하므로 noexcept가 아니다. 기본 생성 뒤 이동 대입으로 옮긴다.
    ProceduralMesh::ProceduralMesh(ProceduralMesh&& other)
    {
        *this = std::move(other);
    }

    ProceduralMesh& ProceduralMesh::operator=(ProceduralMesh&& other) noexcept
    {
        if (this != &other)
        {
            // 원본의 표시를 먼저 끈다: 멤버를 옮기는 도중에 원본이 참인 채로 기하를 잃는 순간이 없게 한다
            const bool validated = other.m_GeometryValidated;
            other.m_GeometryValidated = false;
            m_Geometry = std::move(other.m_Geometry);
            m_GeometryValidated = validated;
            m_SectionSplit = other.m_SectionSplit;
            m_Data = std::move(other.m_Data);
            m_Bounds = other.m_Bounds;
            m_VertexVersion = other.m_VertexVersion;
            m_TopologyVersion = other.m_TopologyVersion;
            other.m_GeometryValidated = false;
        }
        return *this;
    }

    bool ProceduralMesh::SetMesh(ProceduralMeshData data, std::string* error)
    {
        if (!SetRenderData(std::move(data), error))
            return false;
        m_Geometry.Clear();
        m_GeometryValidated = false;
        m_SectionSplit = false;
        return true;
    }

    bool ProceduralMesh::SetRenderData(ProceduralMeshData data, std::string* error)
    {
        if (!data.bindings.empty() && data.bindings.size() != data.vertices.size())
            return Fail(error, "Vertex bindings must be empty or match the vertex count");
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

    bool ProceduralMesh::UpdateVertices(const std::vector<MeshRenderVertex>& vertices, std::string* error)
    {
        if (!m_Geometry.Empty())
            return Fail(error, "Use UpdatePose for a mesh with cutting geometry");
        // 이전 정점과 개수가 다르면 실패 (SetMesh로 초기화를 진행하라)
        if (vertices.size() != m_Data.vertices.size())
            return Fail(error, "UpdateVertices requires the existing vertex count");
        
        // 유효성 검사 및 bounds 설정
        ProceduralMeshBounds bounds;
        if (!ValidateVertices(vertices, bounds, error))
            return false;

        // 실제 데이터 업데이트
        std::vector<MeshRenderVertex> replacement = vertices;
        m_Data.vertices.swap(replacement);
        m_Bounds = bounds;
        m_VertexVersion = NextVersion();
        if (error)
            error->clear();
        return true;
    }

    bool ProceduralMesh::SetTopology(MeshGeometry geometry, std::string* error)
    {
        if (ValidateMeshGeometry(geometry, kDefaultEpsilonScale, error) != SliceStatus::Ok)
            return false;
        return AdoptValidatedTopology(std::move(geometry), error);
    }

    bool ProceduralMesh::AdoptValidatedTopology(MeshGeometry geometry, std::string* error)
    {
#ifdef _DEBUG
        // 검증을 건너뛰는 자리의 계약 확인: 호출자가 넘긴 기하는 이미 기본 epsilon 검증을 통과했어야 한다
        assert(ValidateMeshGeometry(geometry, kDefaultEpsilonScale, nullptr) == SliceStatus::Ok);
#endif
        ProceduralMeshData data;
        BuildRenderMesh(geometry, data);
        if (!SetRenderData(std::move(data), error))
            return false;
        m_Geometry = std::move(geometry);
        m_GeometryValidated = true;
        m_SectionSplit = false;
        return true;
    }

    bool ProceduralMesh::SetSectionSplit(bool splitByOrigin, std::string* error)
    {
        if (m_Geometry.Empty() || m_SectionSplit == splitByOrigin)
        {
            if (error) error->clear();
            return true;
        }
        ProceduralMeshData data;
        RenderBakeOptions options;
        options.splitByOrigin = splitByOrigin;
        BuildRenderMesh(m_Geometry, data, options);
        if (!SetRenderData(std::move(data), error))
            return false;
        m_SectionSplit = splitByOrigin;
        return true;
    }

    bool ProceduralMesh::UpdatePose(const std::vector<std::vector<DirectX::XMFLOAT4X4>>& matrices, std::string* error)
    {
        if (m_Geometry.Empty() || !m_Geometry.skin)
            return Fail(error, "Mesh has no cutting geometry or skin");
        // Skinning needs only vertices and the shared source table. Keep topology in place.
        // Commit geometry, render positions and bounds together after all checks succeed.
        MeshGeometry posed;
        posed.vertices = m_Geometry.vertices;
        posed.skin = m_Geometry.skin;
        posed.skinStateId = m_Geometry.skinStateId;
        if (!posed.skin->Apply(posed, matrices, nullptr, error))
            return false;
        std::vector<MeshRenderVertex> vertices = m_Data.vertices;
        for (size_t i = 0; i < vertices.size(); ++i)
            vertices[i].position = posed.vertices[m_Geometry.corners[i].vertex].position;
        ProceduralMeshBounds bounds;
        if (!ValidateVertices(vertices, bounds, error))
            return false;
        m_Geometry.vertices.swap(posed.vertices);
        m_Data.vertices.swap(vertices);
        m_Bounds = bounds;
        m_GeometryValidated = false;     // 정점이 움직였으므로 면적·축퇴를 다시 확인하지 않은 상태
        m_VertexVersion = NextVersion();
        if (error) error->clear();
        return true;
    }

    void ProceduralMesh::Clear()
    {
        ProceduralMeshData empty;
        std::swap(m_Data, empty);
        m_Geometry.Clear();
        m_GeometryValidated = false;
        m_SectionSplit = false;
        m_Bounds = {};
        m_VertexVersion = NextVersion();
        m_TopologyVersion = NextVersion();
    }
}
