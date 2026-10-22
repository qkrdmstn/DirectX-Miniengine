// 이 파일은 스킨 없는 메시를 절단 입력으로 복사하는 CopyProceduralMeshFromStaticMesh와 강체 위치 갱신 행렬을 구현한다.
#include "pch.h"
#include "StaticMeshCopy.h"
#include "Geometry/Skinning.h"
#include "../Model/Model.h"

#include <DirectXPackedVector.h>
#include <cstring>
#include <unordered_map>

using namespace Math;

namespace MeshCutting
{
    namespace
    {
        bool Fail(std::string* error, const char* message)
        {
            if (error)
                *error = message;
            return false;
        }

        // 스킨 없는 정점 레이아웃: POSITION R32G32B32_FLOAT, NORMAL R10G10B10A2_UNORM, [TANGENT], [UV0 R16G16_FLOAT], [UV1]
        struct StaticVertexLayout
        {
            uint32_t uv0 = UINT32_MAX;
            uint32_t stride = 0;
        };

        StaticVertexLayout ComputeStaticLayout(uint16_t psoFlags)
        {
            StaticVertexLayout layout;
            uint32_t offset = 12 + 4;
            if (psoFlags & PSOFlags::kHasTangent)
                offset += 4;
            if (psoFlags & PSOFlags::kHasUV0)
            {
                layout.uv0 = offset;
                offset += 4;
            }
            if (psoFlags & PSOFlags::kHasUV1)
                offset += 4;
            layout.stride = offset;
            return layout;
        }

        // 정점 하나의 로컬 위치, 로컬 법선, UV를 읽는다.
        void DecodeStatic(const uint8_t* src, const StaticVertexLayout& layout, XMFLOAT3& position, XMFLOAT3& normal, XMFLOAT2& uv)
        {
            std::memcpy(&position, src, sizeof(XMFLOAT3));
            uint32_t packedNormal;
            std::memcpy(&packedNormal, src + 12, sizeof(packedNormal));
            normal = {
                float(packedNormal & 1023) / 1023.0f * 2.0f - 1.0f,
                float((packedNormal >> 10) & 1023) / 1023.0f * 2.0f - 1.0f,
                float((packedNormal >> 20) & 1023) / 1023.0f * 2.0f - 1.0f };
            uv = { 0.0f, 0.0f };
            if (layout.uv0 != UINT32_MAX)
            {
                uint16_t half[2];
                std::memcpy(half, src + layout.uv0, sizeof(half));
                uv = { PackedVector::XMConvertHalfToFloat(half[0]), PackedVector::XMConvertHalfToFloat(half[1]) };
            }
        }

        // 메시 목록에서 meshIndex번째 Mesh를 찾는다 (Mesh는 draw 수에 따라 길이가 다르다).
        const Mesh* FindMesh(const Model& model, uint32_t meshIndex)
        {
            const uint8_t* meshPtr = model.m_MeshData.get();
            for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
            {
                const Mesh& mesh = *(const Mesh*)meshPtr;
                if (i == meshIndex)
                    return &mesh;
                meshPtr += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
            }
            return nullptr;
        }
    }

    void ListStaticMeshes(const Model& model, std::vector<StaticMeshInfo>& out)
    {
        out.clear();
        const uint8_t* meshPtr = model.m_MeshData.get();
        if (meshPtr == nullptr)
            return;
        for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
        {
            const Mesh& mesh = *(const Mesh*)meshPtr;
            meshPtr += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
            if (mesh.psoFlags & PSOFlags::kHasSkin)
                continue;
            StaticMeshInfo info;
            info.meshIndex = i;
            info.meshCBV = mesh.meshCBV;
            info.bounds = { mesh.bounds[0], mesh.bounds[1], mesh.bounds[2], mesh.bounds[3] };
            out.push_back(info);
        }
    }

    bool CopyProceduralMeshFromStaticMesh(const Model& model, const ModelInstance& instance, uint32_t meshIndex,
        ProceduralMeshData& outMesh, BonePatchSource& outSource, std::string* error)
    {
        // 입력 검사: 모델, 자세, 스킨 없는 메시, 레이아웃, 버퍼 범위, 인덱스 형식
        const Matrix4* nodeWorld = instance.GetNodeWorldMatrices();
        if (instance.GetModel() != &model || nodeWorld == nullptr)
            return Fail(error, "Instance has no pose data for this model");
        if (model.m_GeometryCPU.empty())
            return Fail(error, "Model has no CPU geometry copy");
        const Mesh* found = FindMesh(model, meshIndex);
        if (found == nullptr)
            return Fail(error, "Mesh index out of range");
        const Mesh& srcMesh = *found;
        if (srcMesh.psoFlags & PSOFlags::kHasSkin)
            return Fail(error, "Mesh is skinned; use ExtractBonePatch");
        const StaticVertexLayout layout = ComputeStaticLayout(srcMesh.psoFlags);
        if (layout.stride != srcMesh.vbStride)
            return Fail(error, "Unexpected source vertex layout");
        const std::vector<uint8_t>& geometry = model.m_GeometryCPU;
        if (uint64_t(srcMesh.vbOffset) + srcMesh.vbSize > geometry.size()
            || uint64_t(srcMesh.ibOffset) + srcMesh.ibSize > geometry.size())
            return Fail(error, "Mesh buffers exceed geometry data");
        if (srcMesh.meshCBV >= model.m_NumNodes)
            return Fail(error, "Mesh world matrix index out of range");
        const bool index32 = srcMesh.ibFormat == DXGI_FORMAT_R32_UINT;
        if (!index32 && srcMesh.ibFormat != DXGI_FORMAT_R16_UINT)
            return Fail(error, "Unsupported index format");
        const uint32_t indexCount = srcMesh.ibSize / (index32 ? 4u : 2u);
        const uint32_t vertexCount = srcMesh.vbSize / layout.stride;
        const uint8_t* vb = geometry.data() + srcMesh.vbOffset;
        const uint8_t* ib = geometry.data() + srcMesh.ibOffset;

        // 현재 world 행렬과 법선 변환 (BonePatch의 worldIT와 같은 방식)
        XMFLOAT4X4 world;
        XMStoreFloat4x4(&world, XMMATRIX(nodeWorld[srcMesh.meshCBV]));
        const Matrix3 worldIT = InverseTranspose(nodeWorld[srcMesh.meshCBV].Get3x3());

        ProceduralMeshData mesh;
        BonePatchSource source;
        source.paletteId = kNoNode;         // 팔레트 없음
        source.jointIndex = kNoNode;
        std::unordered_map<uint32_t, uint32_t> remap;     // 원본 정점 -> 복사 정점

        for (uint32_t drawIdx = 0; drawIdx < srcMesh.numDraws; ++drawIdx)
        {
            const Mesh::Draw& draw = srcMesh.draw[drawIdx];
            if (draw.primCount % 3 != 0 || draw.startIndex > indexCount || draw.primCount > indexCount - draw.startIndex)
                return Fail(error, "Draw range exceeds index data");
            for (uint32_t i = 0; i < draw.primCount; i += 3)
            {
                for (uint32_t k = 0; k < 3; ++k)
                {
                    // 인덱스 읽기 (16/32비트, baseVertex 적용)
                    const uint32_t at = draw.startIndex + i + k;
                    uint32_t index;
                    if (index32)
                        std::memcpy(&index, ib + size_t(at) * 4, 4);
                    else
                    {
                        uint16_t index16;
                        std::memcpy(&index16, ib + size_t(at) * 2, 2);
                        index = index16;
                    }
                    const uint32_t vertex = draw.baseVertex + index;
                    if (vertex >= vertexCount)
                        return Fail(error, "Index refers beyond the vertex buffer");

                    // 같은 원본 정점은 한 번만 복사한다
                    const auto copied = remap.find(vertex);
                    if (copied != remap.end())
                    {
                        mesh.indices.push_back(copied->second);
                        continue;
                    }
                    XMFLOAT3 localPosition, localNormal;
                    XMFLOAT2 uv;
                    DecodeStatic(vb + size_t(vertex) * layout.stride, layout, localPosition, localNormal, uv);

                    // world 위치는 Skinning::Apply의 강체 정점과 같은 계산이라 첫 갱신과 비트 단위로 맞는다
                    ProceduralVertex out;
                    XMStoreFloat3(&out.position, Skinning::RigidPoint(world, localPosition));
                    const Vector3 worldNormal = Normalize(worldIT * Vector3(localNormal.x, localNormal.y, localNormal.z));
                    out.normal = { float(worldNormal.GetX()), float(worldNormal.GetY()), float(worldNormal.GetZ()) };
                    out.uv = uv;
                    out.color = { 0.8f, 0.8f, 0.8f, 1.0f };     // 회색 (웨이트 색이 없다)

                    BonePatchVertex rigid = {};
                    rigid.meshSlot = 0;
                    rigid.sourceVertex = vertex;
                    rigid.position = localPosition;
                    rigid.normal = localNormal;

                    const uint32_t patchIndex = uint32_t(mesh.vertices.size());
                    mesh.vertices.push_back(out);
                    source.vertices.push_back(rigid);
                    remap.emplace(vertex, patchIndex);
                    mesh.indices.push_back(patchIndex);
                }
                source.triangles.push_back({ 0, drawIdx, i / 3 });
            }
        }
        if (mesh.indices.empty())
            return Fail(error, "Mesh has no triangles");

        source.meshes.push_back({ meshIndex, srcMesh.meshCBV, kNoNode });     // 팔레트가 없어 startJoint는 kNoNode
        mesh.sections.push_back({ 0, uint32_t(mesh.indices.size()), 0 });
        outMesh = std::move(mesh);
        outSource = std::move(source);
        if (error)
            error->clear();
        return true;
    }

    bool BuildRigidMatrices(const Model& model, const ModelInstance& instance, const BonePatchSource& source,
        std::vector<std::vector<XMFLOAT4X4>>& slotMatrices, std::string* error)
    {
        const Matrix4* nodeWorld = instance.GetNodeWorldMatrices();
        if (instance.GetModel() != &model || nodeWorld == nullptr)
            return Fail(error, "Instance has no pose data for this model");
        if (source.meshes.empty())
            return Fail(error, "Source has no mesh");

        // 슬롯마다 그 메시의 현재 world 행렬 하나
        std::vector<std::vector<XMFLOAT4X4>> matrices(source.meshes.size());
        for (size_t slot = 0; slot < source.meshes.size(); ++slot)
        {
            const BonePatchMesh& mesh = source.meshes[slot];
            if (mesh.worldMatrixIndex >= model.m_NumNodes)
                return Fail(error, "Mesh world matrix index out of range");
            matrices[slot].resize(1);
            XMStoreFloat4x4(&matrices[slot][0], XMMATRIX(nodeWorld[mesh.worldMatrixIndex]));
        }
        slotMatrices = std::move(matrices);
        if (error)
            error->clear();
        return true;
    }
}
