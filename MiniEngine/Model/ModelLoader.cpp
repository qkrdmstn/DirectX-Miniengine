//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author(s):  James Stanard
//             Chuck Walbourn (ATG)
//
// This code depends on DirectXTex
//

#include "ModelLoader.h"
#include "Renderer.h"
#include "Model.h"
#include "glTF.h"
#include "ModelH3D.h"
#include "TextureManager.h"
#include "TextureConvert.h"
#include "GraphicsCommon.h"
#include "ReadbackBuffer.h"
#include <string>
#include <cstdint>
#include "Util/CommandLineArg.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <unordered_map>

using namespace Renderer;
using namespace Graphics;

std::unordered_map<uint32_t, uint32_t> g_SamplerPermutations;

D3D12_CPU_DESCRIPTOR_HANDLE GetSampler(uint32_t addressModes)
{
    SamplerDesc samplerDesc;
    samplerDesc.AddressU = D3D12_TEXTURE_ADDRESS_MODE(addressModes & 0x3);
    samplerDesc.AddressV = D3D12_TEXTURE_ADDRESS_MODE(addressModes >> 2);
    return samplerDesc.CreateDescriptor();
}

void LoadMaterials(Model& model,
    const std::vector<MaterialTextureData>& materialTextures,
    const std::vector<std::wstring>& textureNames,
    const std::vector<uint8_t>& textureOptions,
    const std::wstring& basePath)
{
    static_assert((_alignof(MaterialConstants) & 255) == 0, "CBVs need 256 byte alignment");

    // Load textures
    const uint32_t numTextures = (uint32_t)textureNames.size();
    model.textures.resize(numTextures);
    for (size_t ti = 0; ti < numTextures; ++ti)
    {
        std::wstring originalFile = basePath + textureNames[ti];
        CompileTextureOnDemand(originalFile, textureOptions[ti]);

        std::wstring ddsFile = Utility::RemoveExtension(originalFile) + L".dds";
        model.textures[ti] = TextureManager::LoadDDSFromFile(ddsFile);
    }

    // Generate descriptor tables and record offsets for each material
    const uint32_t numMaterials = (uint32_t)materialTextures.size();
    std::vector<uint32_t> tableOffsets(numMaterials);

    for (uint32_t matIdx = 0; matIdx < numMaterials; ++matIdx)
    {
        const MaterialTextureData& srcMat = materialTextures[matIdx];

        DescriptorHandle TextureHandles = Renderer::s_TextureHeap.Alloc(kNumTextures);
        uint32_t SRVDescriptorTable = Renderer::s_TextureHeap.GetOffsetOfHandle(TextureHandles);

        uint32_t DestCount = kNumTextures;
        uint32_t SourceCounts[kNumTextures] = { 1, 1, 1, 1, 1 };

        D3D12_CPU_DESCRIPTOR_HANDLE DefaultTextures[kNumTextures] =
        {
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kWhiteOpaque2D),
            GetDefaultTexture(kBlackTransparent2D),
            GetDefaultTexture(kDefaultNormalMap)
        };

        D3D12_CPU_DESCRIPTOR_HANDLE SourceTextures[kNumTextures];
        for (uint32_t j = 0; j < kNumTextures; ++j)
        {
            if (srcMat.stringIdx[j] == 0xffff)
                SourceTextures[j] = DefaultTextures[j];
            else
                SourceTextures[j] = model.textures[srcMat.stringIdx[j]].GetSRV();
        }

        g_Device->CopyDescriptors(1, &TextureHandles, &DestCount,
            DestCount, SourceTextures, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // See if this combination of samplers has been used before.  If not, allocate more from the heap
        // and copy in the descriptors.
        uint32_t addressModes = srcMat.addressModes;
        auto samplerMapLookup = g_SamplerPermutations.find(addressModes);

        if (samplerMapLookup == g_SamplerPermutations.end())
        {
            DescriptorHandle SamplerHandles = Renderer::s_SamplerHeap.Alloc(kNumTextures);
            uint32_t SamplerDescriptorTable = Renderer::s_SamplerHeap.GetOffsetOfHandle(SamplerHandles);
            g_SamplerPermutations[addressModes] = SamplerDescriptorTable;
            tableOffsets[matIdx] = SRVDescriptorTable | SamplerDescriptorTable << 16;

            D3D12_CPU_DESCRIPTOR_HANDLE SourceSamplers[kNumTextures];
            for (uint32_t j = 0; j < kNumTextures; ++j)
            {
                SourceSamplers[j] = GetSampler(addressModes & 0xF);
                addressModes >>= 4;
            }
            g_Device->CopyDescriptors(1, &SamplerHandles, &DestCount,
                DestCount, SourceSamplers, SourceCounts, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        }
        else
        {
            tableOffsets[matIdx] = SRVDescriptorTable | samplerMapLookup->second << 16;
        }
    }

    // Update table offsets for each mesh
    uint8_t* meshPtr = model.m_MeshData.get();
    for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
    {
        Mesh& mesh = *(Mesh*)meshPtr;
        uint32_t offsetPair = tableOffsets[mesh.materialCBV];
        mesh.srvTable = offsetPair & 0xFFFF;
        mesh.samplerTable = offsetPair >> 16;
        mesh.pso = Renderer::GetPSO(mesh.psoFlags);
        meshPtr += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
    }
}

// TRS 곡선의 대상 노드와 그 하위 노드를 matrixIdx 기준으로 표시한다(skeletonRoot에서 상속 차단). 그래프가 잘못되면 false를 돌려준다.
static bool MarkAnimatedNodes(const Model& model, const AnimationCurve* curves, uint32_t curveCount, std::vector<uint8_t>& animated)
{
    // 결과 배열을 노드 수만큼 비운다.
    const uint32_t count = model.m_NumNodes;
    animated.assign(count, 0);

    // 이동·회전·스케일 곡선이 직접 가리키는 노드를 모은다. weights 곡선은 UpdateAnimations가 노드 변환에 적용하지 않으므로 무시한다.
    // 변환기가 targetNode = linearIdx = matrixIdx = 깊이 우선 인덱스를 보장하므로 targetNode로 찾고 matrixIdx로 저장한다.
    std::vector<uint8_t> targeted(count, 0);
    for (uint32_t i = 0; i < curveCount; ++i)
    {
        const bool isTRS = curves[i].targetPath == AnimationCurve::kTranslation
            || curves[i].targetPath == AnimationCurve::kRotation
            || curves[i].targetPath == AnimationCurve::kScale;
        if (isTRS && curves[i].targetNode < count)
            targeted[curves[i].targetNode] = 1;
    }

    // EvaluateModelPose와 같은 깊이 우선 순서로 부모의 애니메이션 여부를 하위로 전파한다.
    bool parentAnimated = false;
    bool parents[32];
    uint32_t depth = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        const GraphNode& node = model.m_SceneGraph[i];
        if (node.matrixIdx >= count)
            return false;

        // skeletonRoot는 EvaluateModelPose가 부모 변환을 끊으므로 상속된 애니메이션 상태도 끊는다.
        const bool inherited = node.skeletonRoot ? false : parentAnimated;
        const bool self = inherited || targeted[node.matrixIdx] != 0;
        animated[node.matrixIdx] = self ? 1 : 0;

        if (node.hasChildren)
        {
            if (node.hasSibling)
            {
                if (depth == 32)
                    return false;
                parents[depth++] = parentAnimated;
            }
            parentAnimated = self;
        }
        else if (!node.hasSibling)
        {
            if (depth == 0)
                return i + 1 == count;
            parentAnimated = parents[--depth];
        }
    }
    return false;
}

// 메시 정점 위치를 기본 pose의 노드 행렬로 model-local로 옮겨 18-DOP 충돌을 만들고 상태를 돌려준다.
// hull과 허용오차(병합·snap)는 메시 로컬(asset) 단위로 적용되며, 컴포넌트 scale은 body 생성 시 반영된다(1 asset unit = 1 m 가정).
static MeshCollisionStatus BuildOneMeshCollision(const Mesh& mesh, const Model& model, const std::vector<uint8_t>& geometry,
    const Matrix4* nodePoses, MiniCollision::AggregateGeom& out)
{
    // 정점 범위와 레이아웃을 검증한다. 위치는 정점 맨 앞의 float3이다(PreparePoseCS와 동일).
    if (mesh.vbStride < 12 || mesh.vbSize == 0 || mesh.vbSize % mesh.vbStride != 0
        || uint64_t(mesh.vbOffset) + mesh.vbSize > geometry.size() || mesh.meshCBV >= model.m_NumNodes)
        return MeshCollisionStatus::DecodeFailed;

    // 위치를 읽어 노드 행렬로 model-local 좌표로 변환한다(GPU 절단 입력의 SourceFrame⁻¹ 적용 전과 같은 좌표계).
    const Matrix4& nodePose = nodePoses[mesh.meshCBV];
    const uint32_t vertexCount = mesh.vbSize / mesh.vbStride;
    std::vector<MiniCollision::Vec3d> points(vertexCount);
    for (uint32_t v = 0; v < vertexCount; ++v)
    {
        float position[3];
        std::memcpy(position, geometry.data() + mesh.vbOffset + size_t(v) * mesh.vbStride, sizeof(position));
        XMFLOAT4 local;
        XMStoreFloat4(&local, nodePose * Vector3(position[0], position[1], position[2]));
        if (!std::isfinite(local.x) || !std::isfinite(local.y) || !std::isfinite(local.z))
            return MeshCollisionStatus::DecodeFailed;
        points[v] = { local.x, local.y, local.z };
    }

    // 점 집합에서 18-DOP hull을 만든다.
    if (!MiniCollision::GenerateKDop18(points, out))
        return MeshCollisionStatus::GenerateFailed;
    return MeshCollisionStatus::Generated;
}

// 충돌 상태를 로그용 문자열로 바꾼다.
static const char* CollisionStatusName(MeshCollisionStatus status)
{
    // 상태 값 순서와 같은 이름 표를 둔다.
    static const char* const names[] = { "Generated", "Skinned", "NodeAnimated", "DecodeFailed", "GenerateFailed" };

    // 상태를 인덱스로 이름을 돌려준다.
    return names[static_cast<uint32_t>(status)];
}

// 업로드 전 geometry 바이트와 기본 pose로 모든 메시의 충돌을 한 번 만들어 Model에 보관한다. 스킨·노드 애니메이션 메시는 만들지 않는다.
static void BuildMeshCollision(Model& model, const std::vector<uint8_t>& geometry, uint32_t curveCount)
{
    // 결과 배열을 메시 수만큼 준비한다.
    model.m_MeshCollision.assign(model.m_NumMeshes, MiniCollision::AggregateGeom());
    model.m_MeshCollisionStatus.assign(model.m_NumMeshes, MeshCollisionStatus::DecodeFailed);

    // 기본(바인드) pose와 노드별 애니메이션 여부를 계산한다.
    std::vector<Matrix4> nodePoses(model.m_NumNodes);
    std::vector<uint8_t> animated;
    const bool graphOk = EvaluateModelPose(model.m_SceneGraph.get(), model.m_NumNodes, nodePoses.data())
        && MarkAnimatedNodes(model, model.m_CurveData.get(), curveCount, animated);
    if (!graphOk)
        Utility::Printf("Collision: invalid node graph, skipping non-skinned meshes\n");

    // 메시를 순회하며 상태를 정하고 가능한 메시만 hull을 만든다.
    const uint8_t* meshPtr = model.m_MeshData.get();
    for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
    {
        const Mesh& mesh = *reinterpret_cast<const Mesh*>(meshPtr);
        meshPtr += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);

        MeshCollisionStatus status;
        if ((mesh.psoFlags & PSOFlags::kHasSkin) || mesh.numJoints != 0)
            status = MeshCollisionStatus::Skinned;
        else if (!graphOk)
            status = MeshCollisionStatus::DecodeFailed;
        else if (mesh.meshCBV < model.m_NumNodes && animated[mesh.meshCBV])
            status = MeshCollisionStatus::NodeAnimated;
        else
            status = BuildOneMeshCollision(mesh, model, geometry, nodePoses.data(), model.m_MeshCollision[i]);

        model.m_MeshCollisionStatus[i] = status;
        if (status == MeshCollisionStatus::DecodeFailed || status == MeshCollisionStatus::GenerateFailed)
            Utility::Printf("Collision: mesh %u skipped (%s)\n", i, CollisionStatusName(status));
    }

    // -collision-log 1이면 요약과 생성된 메시의 hull 정점 수·부피를 출력한다.
    uint32_t logEnabled = 0;
    if (!CommandLineArgs::GetInteger(L"collision-log", logEnabled) || logEnabled == 0)
        return;

    uint32_t counts[5] = {};
    for (MeshCollisionStatus status : model.m_MeshCollisionStatus)
        ++counts[static_cast<uint32_t>(status)];
    Utility::Printf("Collision: meshes=%u generated=%u skinned=%u nodeAnimated=%u decodeFailed=%u generateFailed=%u\n",
        model.m_NumMeshes, counts[0], counts[1], counts[2], counts[3], counts[4]);
    for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
    {
        if (model.m_MeshCollisionStatus[i] != MeshCollisionStatus::Generated)
            continue;
        const MiniCollision::ConvexElem& hull = model.m_MeshCollision[i].ConvexElems[0];
        Utility::Printf("Collision: mesh %u hullVertices=%u volume=%.4f\n", i, (uint32_t)hull.VertexData.size(), hull.GetVolume());
    }
}

// skin 정점·로컬 삼각형 인덱스와 NodeId별 양수 weight 여부를 보관한다.
static void BuildInfluencingJoints(Model& model, const std::vector<uint8_t>& geometry)
{
    // skin 정점의 마지막 16바이트는 uint16 joint 네 개와 unorm16 weight 네 개다.
    model.m_InfluencingJoints.assign(model.m_NumNodes, 0);
    const uint8_t* meshPtr = model.m_MeshData.get();
    for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
    {
        const Mesh& mesh = *reinterpret_cast<const Mesh*>(meshPtr);
        meshPtr += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
        if (!mesh.numJoints) continue;
        model.m_SkinCpuSections.emplace_back();
        auto& vertices = model.m_SkinCpuSections.back();
        vertices.StartJoint = mesh.startJoint;
        vertices.NumJoints = mesh.numJoints;
        if (mesh.vbStride < 28 || mesh.vbSize % mesh.vbStride || uint64_t(mesh.vbOffset) + mesh.vbSize > geometry.size())
            continue;
        vertices.resize(mesh.vbSize / mesh.vbStride);

        // R16/R32 draw 범위를 해석해 section VB 기준의 인덱스로 보존한다.
        const uint32_t indexSize = mesh.ibFormat == DXGI_FORMAT_R16_UINT ? 2 : 4;
        if ((mesh.ibFormat != DXGI_FORMAT_R16_UINT && mesh.ibFormat != DXGI_FORMAT_R32_UINT) ||
            uint64_t(mesh.ibOffset) + mesh.ibSize > geometry.size())
            throw std::runtime_error("skin index buffer");
        for (uint32_t d = 0; d < mesh.numDraws; ++d)
        {
            const auto& draw = mesh.draw[d];
            if (draw.primCount % 3 || (uint64_t(draw.startIndex) + draw.primCount) * indexSize > mesh.ibSize)
                throw std::runtime_error("skin draw indices");
            for (uint32_t index = 0; index < draw.primCount; ++index)
            {
                uint32_t local = 0;
                std::memcpy(&local, geometry.data() + mesh.ibOffset +
                    (size_t(draw.startIndex) + index) * indexSize, indexSize);
                const int64_t vertex = int64_t(local) + int32_t(draw.baseVertex);
                if (vertex < 0 || uint64_t(vertex) >= vertices.size())
                    throw std::runtime_error("skin local index");
                vertices.Indices.push_back(uint32_t(vertex));
            }
        }

        // 양수 합으로 나눈 weight의 양수 여부는 저장된 정수 weight의 양수 여부와 같다.
        for (uint32_t v = 0; v < mesh.vbSize / mesh.vbStride; ++v)
        {
            auto& vertex = vertices[v];
            const auto* source = geometry.data() + mesh.vbOffset + size_t(v) * mesh.vbStride;
            std::memcpy(vertex.Position, source, sizeof(vertex.Position));
            std::memcpy(vertex.Joints, source + mesh.vbStride - 16, sizeof(vertex.Joints));
            std::memcpy(vertex.Weights, source + mesh.vbStride - 8, sizeof(vertex.Weights));
            for (uint32_t j = 0; j < 4; ++j)
                if (vertex.Weights[j] > 0 && vertex.Joints[j] < mesh.numJoints)
                {
                    const uint32_t palette = mesh.startJoint + vertex.Joints[j];
                    if (palette < model.m_NumJoints && model.m_JointIndices[palette] < model.m_NumNodes)
                        model.m_InfluencingJoints[model.m_JointIndices[palette]] = 1;
                }
        }
    }
}

// skin 격자의 section별 셀 번호를 한 번 업로드하고 요청한 경우 CPU oracle과 대조한다.
static void UploadLatticeTopology(Model& model)
{
    // 정적 모델이나 빈 격자는 GPU 자료를 만들지 않는다.
    const auto& cpu = model.m_LatticeTopology;
    if (model.m_SkinCpuSections.empty() || cpu.CellCount == 0)
        return;

    // section 인덱스를 보존하고 초기 데이터 복사는 로드 시 동기 완료한다.
    auto& gpu = model.m_GpuLatticeTopology;
    gpu.SectionVertexCell.resize(cpu.VertexCell.size());
    gpu.CellCount = cpu.CellCount;
    for (size_t section = 0; section < cpu.VertexCell.size(); ++section)
    {
        // 빈 section은 슬롯만 남겨 0바이트 리소스 생성을 피한다.
        const auto& cells = cpu.VertexCell[section];
        if (cells.empty())
            continue;

        // SIMD 복사가 올림하는 16바이트 범위까지 임시 원본을 확보한다.
        auto uploadCells = cells;
        uploadCells.resize((cells.size() + 3) & ~size_t(3), 0);
        gpu.SectionVertexCell[section].Create(L"Lattice VertexCell " + std::to_wstring(section),
            uint32_t(cells.size()), sizeof(uint32_t), uploadCells.data());
    }

    // 진단 옵션을 켠 로드에서만 section별 회수와 전체 일치 수를 기록한다.
    uint32_t verify = 0;
    CommandLineArgs::GetInteger(L"island-gpu-verify", verify);
    if (verify == 0)
        return;
    uint64_t vertices = 0, mismatches = 0, bytes = 0;
    for (size_t section = 0; section < cpu.VertexCell.size(); ++section)
    {
        // 업로드 버퍼를 회수한 뒤 향후 compute SRV 상태로 복원한다.
        const auto& cells = cpu.VertexCell[section];
        if (cells.empty())
            continue;
        auto& buffer = gpu.SectionVertexCell[section];
        ReadbackBuffer readback;
        readback.Create(L"Lattice VertexCell verify", uint32_t(cells.size()), sizeof(uint32_t));
        auto& context = CommandContext::Begin(L"Verify lattice upload");
        context.CopyBuffer(readback, buffer);
        context.TransitionResource(buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.Finish(true);

        // Map 실패도 불일치로 세고 성공한 Map은 비교 직후 해제한다.
        const uint32_t* actual = nullptr;
        try
        {
            actual = static_cast<const uint32_t*>(readback.Map());
        }
        catch (const std::exception& error)
        {
            Utility::Printf("GpuLatticeTopology verify FAIL section=%u Map: %s\n",
                uint32_t(section), error.what());
        }

        // Map 실패는 해당 section의 모든 정점을 불일치로 집계한다.
        uint64_t different = 0;
        for (size_t vertex = 0; vertex < cells.size(); ++vertex)
            different += !actual || actual[vertex] != cells[vertex];
        if (actual)
            readback.Unmap();
        vertices += cells.size();
        bytes += buffer.GetBufferSize();
        mismatches += different;
        Utility::Printf("GpuLatticeTopology section=%u vertices=%u bytes=%llu mismatches=%llu\n",
            uint32_t(section), uint32_t(cells.size()), uint64_t(buffer.GetBufferSize()), different);
    }

    // 모델 식별 로그와 함께 읽을 수 있도록 로드당 최종 판정을 남긴다.
    Utility::Printf("GpuLatticeTopology verify %s sections=%u vertices=%llu bytes=%llu CellCount=%u mismatches=%llu\n",
        mismatches == 0 ? "PASS" : "FAIL", uint32_t(cpu.VertexCell.size()), vertices, bytes, gpu.CellCount, mismatches);
}

// .mini를 읽고 로드 시 충돌과 영향 joint 캐시를 만든 뒤 GPU에 업로드한다.
std::shared_ptr<Model> Renderer::LoadModel(const std::wstring& filePath, bool forceRebuild)
{
    // 입력 모델 이름과 캐시 경로를 정한다.
    const std::wstring miniFileName = Utility::RemoveExtension(filePath) + L".mini";
    const std::wstring fileName = Utility::RemoveBasePath(filePath);

    struct _stat64 sourceFileStat;
    struct _stat64 miniFileStat;
    std::ifstream inFile;
    FileHeader header;

    bool sourceFileMissing = _wstat64(filePath.c_str(), &sourceFileStat) == -1;
    bool miniFileMissing = _wstat64(miniFileName.c_str(), &miniFileStat) == -1;

    if (sourceFileMissing)
        forceRebuild = false;

    if (sourceFileMissing && miniFileMissing)
    {
        Utility::Printf("Error: Could not find %ws\n", fileName.c_str());
        return nullptr;
    }

    bool needBuild = forceRebuild;

    // Check if .mini file exists and it is newer than source file
    if (miniFileMissing || !sourceFileMissing && sourceFileStat.st_mtime > miniFileStat.st_mtime)
        needBuild = true;

    // Check if it's an older version of .mini
    if (!needBuild)
    {
        inFile = std::ifstream(miniFileName, std::ios::in | std::ios::binary);
        inFile.read((char*)&header, sizeof(FileHeader));
        if (strncmp(header.id, "MINI", 4) != 0 || header.version != CURRENT_MINI_FILE_VERSION)
        {
            Utility::Printf("Model version deprecated.  Rebuilding %ws...\n", fileName.c_str());
            needBuild = true;
            inFile.close();
        }
    }

    if (needBuild)
    {
        if (sourceFileMissing)
        {
            Utility::Printf("Error: Could not find %ws\n", fileName.c_str());
            return nullptr;
        }

        ModelData modelData;

        const std::wstring fileExt = Utility::ToLower(Utility::GetFileExtension(filePath));

        if (fileExt == L"gltf" || fileExt == L"glb")
        {
            glTF::Asset asset(filePath);
            if (!BuildModel(modelData, asset))
                return nullptr;
        }
        else if (fileExt == L"h3d")
        {
            ModelH3D modelh3d;
            const std::wstring basePath = Utility::GetBasePath(filePath);
            if (!modelh3d.Load(filePath) || !modelh3d.BuildModel(modelData, basePath))
                return nullptr;
        }
        else
        {
            Utility::Printf(L"Unsupported model file extension: %ws\n", fileExt.c_str());
            return nullptr;
        }

        if (!SaveModel(miniFileName, modelData))
            return nullptr;

        inFile = std::ifstream(miniFileName, std::ios::in | std::ios::binary);
        inFile.read((char*)&header, sizeof(FileHeader));
    }

    if (!inFile)
        return nullptr;

    ASSERT(strncmp(header.id, "MINI", 4) == 0 && header.version == CURRENT_MINI_FILE_VERSION);

    std::wstring basePath = Utility::GetBasePath(filePath);

    std::shared_ptr<Model> model(new Model);

    model->m_NumNodes = header.numNodes;
    model->m_SceneGraph.reset(new GraphNode[header.numNodes]);
    model->m_NumMeshes = header.numMeshes;
    model->m_MeshData.reset(new uint8_t[header.meshDataSize]);

    // geometry 바이트는 충돌 계산 후 업로드하도록 CPU 메모리에 보관한다(파일 읽기 순서는 그대로).
    std::vector<uint8_t> geometryBytes(header.geometrySize);
    if (header.geometrySize > 0)
        inFile.read((char*)geometryBytes.data(), header.geometrySize);

    inFile.read((char*)model->m_SceneGraph.get(), header.numNodes * sizeof(GraphNode));
    inFile.read((char*)model->m_MeshData.get(), header.meshDataSize);

	if (header.numMaterials > 0)
	{
		UploadBuffer materialConstants;
		materialConstants.Create(L"Material Constant Upload", header.numMaterials * sizeof(MaterialConstants));
		MaterialConstants* materialCBV = (MaterialConstants*)materialConstants.Map();
		for (uint32_t i = 0; i < header.numMaterials; ++i)
		{
			inFile.read((char*)materialCBV, sizeof(MaterialConstantData));
			materialCBV++;
		}
		materialConstants.Unmap();
		model->m_MaterialConstants.Create(L"Material Constants", header.numMaterials, sizeof(MaterialConstants), materialConstants);
	}

    // Read material texture and sampler properties so we can load the material
    std::vector<MaterialTextureData> materialTextures(header.numMaterials);
    inFile.read((char*)materialTextures.data(), header.numMaterials * sizeof(MaterialTextureData));

    std::vector<std::wstring> textureNames(header.numTextures);
    for (uint32_t i = 0; i < header.numTextures; ++i)
    {
        std::string utf8TextureName;
        std::getline(inFile, utf8TextureName, '\0');
        textureNames[i] = Utility::UTF8ToWideString(utf8TextureName);
    }

    std::vector<uint8_t> textureOptions(header.numTextures);
    inFile.read((char*)textureOptions.data(), header.numTextures * sizeof(uint8_t));

    LoadMaterials(*model, materialTextures, textureNames, textureOptions, basePath);

    model->m_BoundingSphere = BoundingSphere(*(XMFLOAT4*)header.boundingSphere);
    model->m_BoundingBox = AxisAlignedBox(Vector3(*(XMFLOAT3*)header.minPos), Vector3(*(XMFLOAT3*)header.maxPos));

    // Load animation data
    model->m_NumAnimations = header.numAnimations;

    if (header.numAnimations > 0)
    {
        ASSERT(header.keyFrameDataSize > 0 && header.numAnimationCurves > 0);
        model->m_KeyFrameData.reset(new uint8_t[header.keyFrameDataSize]);
        inFile.read((char*)model->m_KeyFrameData.get(), header.keyFrameDataSize);
        model->m_CurveData.reset(new AnimationCurve[header.numAnimationCurves]);
        inFile.read((char*)model->m_CurveData.get(), header.numAnimationCurves * sizeof(AnimationCurve));
        model->m_Animations.reset(new AnimationSet[header.numAnimations]);
        inFile.read((char*)model->m_Animations.get(), header.numAnimations * sizeof(AnimationSet));
    }

    model->m_NumJoints = header.numJoints;

    if (header.numJoints > 0)
    {
        model->m_JointIndices.reset(new uint16_t[header.numJoints]);
        inFile.read((char*)model->m_JointIndices.get(), header.numJoints * sizeof(uint16_t));
        model->m_JointIBMs.reset(new Matrix4[header.numJoints]);
        inFile.read((char*)model->m_JointIBMs.get(), header.numJoints * sizeof(Matrix4));
    }

    // C 경로에서 정점을 읽지 않도록 모델 로드 시 영향 joint를 확정한다.
    BuildInfluencingJoints(*model, geometryBytes);

    // skin이 있는 모델은 bind 격자를 한 번 만들고 연결 통계를 기록한다.
    if (!model->m_SkinCpuSections.empty())
    {
        // 기본 node 계층을 기존 pose 평가기로 계산하고 각 skin의 IBM을 합성한다.
        std::vector<Matrix4> restNodes(model->m_NumNodes);
        if (!EvaluateModelPose(model->m_SceneGraph.get(), model->m_NumNodes, restNodes.data()))
            throw std::runtime_error("lattice rest node graph");
        std::vector<LatticeRestTransform> restTransforms(model->m_NumJoints);
        for (uint32_t joint = 0; joint < model->m_NumJoints; ++joint)
        {
            // 같은 node를 참조해도 skin마다 다른 IBM과 팔레트 항목을 보존한다.
            if (model->m_JointIndices[joint] >= model->m_NumNodes)
                throw std::runtime_error("lattice rest joint node");
            const Matrix4 transform = restNodes[model->m_JointIndices[joint]] * model->m_JointIBMs[joint];
            DirectX::XMFLOAT4X4 stored;
            DirectX::XMStoreFloat4x4(&stored, transform);
            std::memcpy(restTransforms[joint].data(), &stored, sizeof(stored));
        }

        // 원본 mesh-local 위치는 유지하고 공통 모델 공간 rest 위치만 격자에 쓴다.
        model->m_LatticeTopology = BuildLatticeTopology(model->m_SkinCpuSections, 64, restTransforms);
        UploadLatticeTopology(*model);
        const auto& lattice = model->m_LatticeTopology;
        Utility::Printf("LatticeTopology: Dim=%ux%ux%u cells=%u edges=%u components=%u\n",
            lattice.Dim[0], lattice.Dim[1], lattice.Dim[2], lattice.CellCount,
            uint32_t(lattice.Edges.size()), CountLatticeComponents(lattice));
    }

    // 파일을 모두 읽은 뒤 업로드 전에 메시별 충돌을 CPU에서 계산한다.
    BuildMeshCollision(*model, geometryBytes, model->m_CurveData ? header.numAnimationCurves : 0);

    // 계산이 끝난 geometry를 GPU 버퍼로 업로드한다.
    if (header.geometrySize > 0)
    {
        UploadBuffer modelData;
        modelData.Create(L"Model Data Upload", header.geometrySize);
        std::memcpy(modelData.Map(), geometryBytes.data(), header.geometrySize);
        modelData.Unmap();
        model->m_DataBuffer.Create(L"Model Data", header.geometrySize, 1, modelData);
    }

    return model;
}
