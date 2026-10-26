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
#include <string>
#include <cstdint>
#include "Util/CommandLineArg.h"

#include <cmath>
#include <cstring>
#include <fstream>
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

// .mini를 읽어 Model을 만든다. geometry는 CPU에 보관해 메시별 충돌을 계산한 뒤 GPU로 업로드한다.
std::shared_ptr<Model> Renderer::LoadModel(const std::wstring& filePath, bool forceRebuild)
{
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
