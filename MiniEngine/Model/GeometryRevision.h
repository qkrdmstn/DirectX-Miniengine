#pragma once

#include "GeometryBuffers.h"
#include <atomic>
#include <limits>
#include <stdexcept>

// 원본 GPU 구간과 출처를 함께 보관한다. 렌더 정점 전체를 CPU로 복사하지 않는다.
struct GeometrySection
{
    uint32_t SourceMesh;
    uint32_t MeshByteOffset;
};

// 게시 후 변경하지 않는 geometry. 원본 section 참조와 생성 GPU geometry를 같은 revision으로 관리한다.
class GeometryRevision
{
public:
    // 공유 원본의 section·VB/IB·재질·출처를 묶어 최초 revision을 만든다.
    static std::shared_ptr<const GeometryRevision> FromModel(std::shared_ptr<const Model> model)
    {
        if (!model || (model->m_NumMeshes > 0 && !model->m_MeshData))
            throw std::invalid_argument("Geometry requires a source model");
        std::vector<GeometrySection> sections;
        size_t offset = 0;
        for (uint32_t i = 0; i < model->m_NumMeshes; ++i)
        {
            const Mesh& mesh = *reinterpret_cast<const Mesh*>(model->m_MeshData.get() + offset);
            if (mesh.numDraws == 0 || mesh.meshCBV >= model->m_NumNodes)
                throw std::invalid_argument("Invalid geometry section");
            if (offset > std::numeric_limits<uint32_t>::max())
                throw std::overflow_error("Mesh metadata exceeds 4 GiB");
            sections.push_back({ i, static_cast<uint32_t>(offset) });
            offset += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
        }
        return std::shared_ptr<const GeometryRevision>(new GeometryRevision(std::move(model), std::move(sections)));
    }

    // 현재 section의 부분집합을 새 revision으로 만든다. 이전 revision과 GPU 자원은 유지한다.
    std::shared_ptr<const GeometryRevision> SelectSections(const std::vector<uint32_t>& indices) const
    {
        std::vector<GeometrySection> sections;
        std::vector<bool> selected(m_Sections.size());
        for (uint32_t index : indices)
        {
            if (index >= m_Sections.size() || selected[index])
                throw std::invalid_argument("Invalid or duplicate section");
            selected[index] = true;
            sections.push_back(m_Sections[index]);
        }
        auto result = std::shared_ptr<GeometryRevision>(new GeometryRevision(m_Source, std::move(sections)));
        result->m_Generated = m_Generated;
        return result;
    }

    // GPU 작업 완료 후 생성 geometry를 불변 revision으로 게시한다.
    static std::shared_ptr<const GeometryRevision> FromGenerated(std::shared_ptr<GeometryBuffers> buffers)
    {
        if (!buffers || !buffers->Source || buffers->Meshes.size() != buffers->PoseOffsets.size())
            throw std::invalid_argument("Invalid generated geometry");
        for (size_t i = 0; i < buffers->Meshes.size(); ++i)
        {
            const Mesh& mesh = buffers->Meshes[i];
            const uint64_t vertices = mesh.vbSize / 28;
            if (mesh.meshCBV != 0 || mesh.numJoints != 0 || (mesh.psoFlags & PSOFlags::kHasSkin)
                || (mesh.psoFlags & 11) != 11 || mesh.numDraws != 1 || mesh.vbStride != 28 || mesh.ibFormat != DXGI_FORMAT_R32_UINT
                || mesh.vbSize % 28 || mesh.ibSize % 4 || mesh.draw[0].primCount % 3
                || uint64_t(mesh.draw[0].startIndex) + mesh.draw[0].primCount > mesh.ibSize / 4
                || mesh.draw[0].baseVertex != 0
                || uint64_t(mesh.vbOffset) + mesh.vbSize > buffers->Data.GetBufferSize()
                || uint64_t(mesh.ibOffset) + mesh.ibSize > buffers->Data.GetBufferSize()
                || uint64_t(mesh.vbDepthOffset) + mesh.vbDepthSize > buffers->Data.GetBufferSize()
                || uint64_t(buffers->PoseOffsets[i]) + vertices * 40 > buffers->Pose.GetBufferSize()
                || (mesh.materialCBV == 0xffff ? buffers->CapMaterial.GetBufferSize() < 256
                    : mesh.materialCBV * 256ull + 256 > buffers->Source->m_MaterialConstants.GetBufferSize()))
                throw std::invalid_argument("Invalid generated draw descriptor");
        }
        auto result = std::shared_ptr<GeometryRevision>(new GeometryRevision(buffers->Source, {}));
        for (uint32_t i = 0; i < buffers->Meshes.size(); ++i)
            result->m_Sections.push_back({ i, 0 });
        result->m_Generated = std::move(buffers);
        return result;
    }
    // 생성 geometry이면 node·skin·source 프레임을 다시 적용하지 않는다.
    bool IsGenerated() const { return bool(m_Generated); }
    // 원본/생성 geometry가 사용하는 정점·인덱스 버퍼를 읽는다.
    const ByteAddressBuffer& GetDataBuffer() const { return m_Generated ? m_Generated->Data : m_Source->m_DataBuffer; }
    // 원본 표면은 원본 재질을 유지하고 cap은 독립 재질 상수를 사용한다.
    D3D12_GPU_VIRTUAL_ADDRESS GetMaterialAddress(const Mesh& mesh) const
    {
        return m_Generated && mesh.materialCBV == 0xffff ? m_Generated->CapMaterial.GetGpuVirtualAddress()
            : m_Source->m_MaterialConstants.GetGpuVirtualAddress() + mesh.materialCBV * 256ull;
    }
    // GPU 결과 저장 공간을 읽는다. 원본 revision에서는 null이다.
    const std::shared_ptr<const GeometryBuffers>& GetGenerated() const { return m_Generated; }
    // revision 식별자를 읽는다. 물리 cooking 결과의 유효성 검사에도 사용한다.
    uint64_t GetId() const { return m_Id; }
    // 원본 소유권을 공유해 GPU geometry·재질·텍스처 수명을 보장한다.
    const std::shared_ptr<const Model>& GetSource() const { return m_Source; }
    // 이 revision이 실제로 그리는 section 목록을 읽는다.
    const std::vector<GeometrySection>& GetSections() const { return m_Sections; }
    // section의 VB/IB 구간·draw·재질 참조를 읽는다.
    const Mesh& GetMesh(size_t sectionIndex) const
    {
        const auto& section = m_Sections.at(sectionIndex);
        return m_Generated ? m_Generated->Meshes.at(section.SourceMesh)
            : *reinterpret_cast<const Mesh*>(m_Source->m_MeshData.get() + section.MeshByteOffset);
    }

private:
    // 불변 section 목록과 원본 소유권을 함께 게시한다.
    GeometryRevision(std::shared_ptr<const Model> source, std::vector<GeometrySection> sections)
        : m_Source(std::move(source)), m_Sections(std::move(sections))
    {
        static std::atomic<uint64_t> next{ 1 };
        m_Id = next.load();
        do
        {
            if (m_Id == std::numeric_limits<uint64_t>::max())
                throw std::overflow_error("Geometry revision IDs exhausted");
        } while (!next.compare_exchange_weak(m_Id, m_Id + 1));
    }

    std::shared_ptr<const GeometryBuffers> m_Generated;
    uint64_t m_Id;
    std::shared_ptr<const Model> m_Source;
    std::vector<GeometrySection> m_Sections;
};
