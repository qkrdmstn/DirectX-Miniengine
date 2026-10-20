#include "pch.h"
#include "BonePatch.h"
#include "Geometry/JointSet.h"
#include "../Model/Model.h"

#include <DirectXPackedVector.h>
#include <algorithm>
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

        // 미니엔진이 모델을 최초 load할 때에만 정점을 제공하고, 이후부턴 압축된 바이트 형태로 m_GeometryCPU 형태로 다루기 때문에 이를 해석하기 위한 Layout이 필요하다.
        // 원본 메시의 정점 정보를 읽기 위한 레이아웃
        struct VertexLayout
        {
            uint32_t uv0 = UINT32_MAX;
            uint32_t joints = 0;
            uint32_t weights = 0;
            uint32_t stride = 0;
        };

        // 원본 메시 데이터의 메모리 레이아웃 계산
        VertexLayout ComputeLayout(uint16_t psoFlags)
        {
            VertexLayout layout;
            uint32_t offset = 12 + 4; // POSITION R32G32B32_FLOAT, NORMAL R10G10B10A2_UNORM
            if (psoFlags & PSOFlags::kHasTangent)
                offset += 4;
            if (psoFlags & PSOFlags::kHasUV0)
            {
                layout.uv0 = offset;
                offset += 4;
            }
            if (psoFlags & PSOFlags::kHasUV1)
                offset += 4;
            layout.joints = offset;     // R16G16B16A16_UINT
            layout.weights = offset + 8; // R16G16B16A16_UNORM
            layout.stride = offset + 16;
            return layout;
        }

        // 원본으로부터 읽고, Procedural Mesh로 구성되기 전의 임시 저장소
        struct DecodedVertex
        {
            XMFLOAT3 position;
            XMFLOAT3 normal;
            XMFLOAT2 uv;
            uint16_t joints[4];
            float weights[4];
        };

        // 계산한 레이아웃을 이용해 Vertex 하나의 정보를 디코딩한다.
        DecodedVertex Decode(const uint8_t* src, const VertexLayout& layout)
        {
            DecodedVertex v = {};
            std::memcpy(&v.position, src, sizeof(XMFLOAT3));

            // 압축된 법선을 복원
            uint32_t packedNormal;
            std::memcpy(&packedNormal, src + 12, sizeof(packedNormal));
            v.normal = {
                float(packedNormal & 1023) / 1023.0f * 2.0f - 1.0f,
                float((packedNormal >> 10) & 1023) / 1023.0f * 2.0f - 1.0f,
                float((packedNormal >> 20) & 1023) / 1023.0f * 2.0f - 1.0f };

            // uv 정보가 포함되어 있으면 읽기
            if (layout.uv0 != UINT32_MAX)
            {
                uint16_t half[2];
                std::memcpy(half, src + layout.uv0, sizeof(half));
                v.uv = { PackedVector::XMConvertHalfToFloat(half[0]), PackedVector::XMConvertHalfToFloat(half[1]) };
            }

            // bone 번호와 해당하는 각 가중치 읽기
            uint16_t weights[4];
            std::memcpy(v.joints, src + layout.joints, sizeof(v.joints));
            std::memcpy(weights, src + layout.weights, sizeof(weights));
            float sum = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                v.weights[i] = float(weights[i]) / 65535.0f;
                sum += v.weights[i];
            }

            // 가중치의 합이 1이 되도록 정리
            for (int i = 0; i < 4; ++i)
                v.weights[i] = sum > 0.0f ? v.weights[i] / sum : 0.0f;

            return v;
        }
    }

    void MakeSlotMatrices(const Matrix4& meshWorld, const Joint* paletteJoints, uint32_t jointCount,
        std::vector<XMFLOAT4X4>& out)
    {
        out.resize(jointCount);

        // 각 관절의 스키닝 행렬에 메시 월드 행렬 결합
        for (uint32_t j = 0; j < jointCount; ++j)
            XMStoreFloat4x4(&out[j], XMMATRIX(meshWorld * paletteJoints[j].posXform));
    }

    bool BuildSkinMatrices(const Model& model, const ModelInstance& instance, const BonePatchSource& source,
        std::vector<std::vector<XMFLOAT4X4>>& slotMatrices, std::string* error)
    {
        // 모델, 자세, 팔레트 유효성 검사
        const Joint* skeleton = instance.GetSkeleton();
        const Matrix4* nodeWorld = instance.GetNodeWorldMatrices();
        if (instance.GetModel() != &model || skeleton == nullptr || nodeWorld == nullptr)
            return Fail(error, "Instance has no pose data for this model");
        if (source.paletteId >= model.m_SkinPalettes.size())
            return Fail(error, "Skin palette out of range");

        // 각 메시마다 스키닝 행렬 결합
        const SkinPalette& palette = model.m_SkinPalettes[source.paletteId];
        slotMatrices.resize(source.meshes.size());
        for (size_t slot = 0; slot < source.meshes.size(); ++slot)
        {
            const BonePatchMesh& mesh = source.meshes[slot];
            if (mesh.worldMatrixIndex >= model.m_NumNodes || mesh.startJoint != palette.startJoint)
                return Fail(error, "Patch mesh does not match the skin palette");
            MakeSlotMatrices(nodeWorld[mesh.worldMatrixIndex], skeleton + palette.startJoint, palette.numJoints, slotMatrices[slot]);
        }
        return true;
    }

    bool ComputePoseJointWorld(const Model& model, const ModelInstance& instance, const BonePatchSource& source,
        std::vector<Matrix4>& out, std::string* error)
    {
        // 패치의 팔레트와 기준 메시 검사
        const Matrix4* nodeWorld = instance.GetNodeWorldMatrices();
        if (instance.GetModel() != &model || nodeWorld == nullptr || source.meshes.empty())
            return Fail(error, "Instance has no pose data for this patch");
        if (source.paletteId >= model.m_SkinPalettes.size())
            return Fail(error, "Skin palette out of range");
        const SkinPalette& palette = model.m_SkinPalettes[source.paletteId];
        if (source.meshes[0].worldMatrixIndex >= model.m_NumNodes)
            return Fail(error, "Patch mesh world matrix index out of range");

        const Matrix4& world = nodeWorld[source.meshes[0].worldMatrixIndex];
        out.resize(palette.numJoints);
        for (uint32_t j = 0; j < palette.numJoints; ++j)
        {
            // 각 joint에 대응하는 모델 노드 찾기
            const uint32_t jointNode = model.m_JointIndices[palette.startJoint + j];
            if (jointNode >= model.m_NumNodes)
                return Fail(error, "Joint node index out of range");
            out[j] = world * nodeWorld[jointNode];
        }
        return true;
    }

    bool ExtractBonePatch(const Model& model, const ModelInstance& instance,
        uint32_t paletteId, uint32_t jointIndex, float minWeight,
        ProceduralMeshData& outMesh, BonePatchSource& outSource, std::string* error)
    {
        return ExtractBonePatch(model, instance, paletteId, std::vector<uint32_t>{ jointIndex }, minWeight,
            outMesh, outSource, error);
    }

    // 추출삼각형의 정점 중 하나라도 선택 관절의 가중치가 minWeight를 넘으면 삼각형을 복사한다.
    // 자손 관절에만 영향을 받는 삼각형은 원본에 남긴다. 실제 분리 기능이 이 함수에 있는건 아니다.
    // 인스턴스의 마지막 Update()에서 계산한 행렬로 자세를 적용, 결과 위치는 월드 좌표이므로, 그릴 때 주의해야 한다.
    bool ExtractBonePatch(const Model& model, const ModelInstance& instance,
        uint32_t paletteId, const std::vector<uint32_t>& joints, float minWeight,
        ProceduralMeshData& outMesh, BonePatchSource& outSource, std::string* error)
    {
        // 입력 모델, 팔레트, 조인트 목록 검사
        if (instance.GetModel() != &model)
            return Fail(error, "Instance does not belong to the model");
        if (paletteId >= model.m_SkinPalettes.size())
            return Fail(error, "Skin palette out of range");
        const SkinPalette& palette = model.m_SkinPalettes[paletteId];
        if (!IsValidJointSet(joints, palette.numJoints))
            return Fail(error, "Joint set is empty, unsorted or out of range for the palette");
        if (model.m_GeometryCPU.empty())
            return Fail(error, "Model has no CPU geometry copy");
        const Joint* skeleton = instance.GetSkeleton();
        const Matrix4* nodeWorld = instance.GetNodeWorldMatrices();
        if (skeleton == nullptr || nodeWorld == nullptr)
            return Fail(error, "Instance has no pose data");

        // 선택 조인트들에 대한 자식 관계 확인
        std::vector<uint8_t> inSubtree(palette.numJoints, 0);
        {
            std::unordered_map<uint32_t, uint32_t> nodeToJoint;
            for (uint32_t j = 0; j < palette.numJoints; ++j)
                nodeToJoint.emplace(model.m_JointIndices[palette.startJoint + j], j);
            for (uint32_t j = 0; j < palette.numJoints; ++j)
            {
                uint32_t node = model.m_JointIndices[palette.startJoint + j];
                // Bounded by the node count so a malformed hierarchy cannot loop forever.
                for (uint32_t depth = 0; depth <= model.m_NumNodes && node < model.m_NodeParents.size(); ++depth)
                {
                    const auto found = nodeToJoint.find(node);
                    if (found != nodeToJoint.end() && std::binary_search(joints.begin(), joints.end(), found->second))
                    {
                        inSubtree[j] = 1;
                        break;
                    }
                    node = model.m_NodeParents[node];
                }
            }
        }

        // 출력 형식
        const std::vector<uint8_t>& geometry = model.m_GeometryCPU;
        ProceduralMeshData mesh;
        BonePatchSource source;
        source.paletteId = paletteId;
        source.jointIndex = joints.front();
        source.joints = joints;
        source.minWeight = minWeight;

        Matrix4 poseWorld = Matrix4(kIdentity);     // world of the mesh that defined poseJointWorld
        bool havePoseWorld = false;
        const uint8_t* meshPtr = model.m_MeshData.get();
        for (uint32_t meshIndex = 0; meshIndex < model.m_NumMeshes; ++meshIndex)
        {
            const Mesh& srcMesh = *(const Mesh*)meshPtr;
            meshPtr += sizeof(Mesh) + (srcMesh.numDraws - 1) * sizeof(Mesh::Draw);

            if ((srcMesh.psoFlags & PSOFlags::kHasSkin) == 0 || srcMesh.numJoints == 0
                || srcMesh.startJoint != palette.startJoint)
                continue;

            const VertexLayout layout = ComputeLayout(srcMesh.psoFlags);
            if (layout.stride != srcMesh.vbStride)
                return Fail(error, "Unexpected source vertex layout");
            if (uint64_t(srcMesh.vbOffset) + srcMesh.vbSize > geometry.size()
                || uint64_t(srcMesh.ibOffset) + srcMesh.ibSize > geometry.size())
                return Fail(error, "Mesh buffers exceed geometry data");
            if (srcMesh.meshCBV >= model.m_NumNodes)
                return Fail(error, "Mesh world matrix index out of range");
            if (srcMesh.numJoints > palette.numJoints)
                return Fail(error, "Mesh uses more joints than its palette");

            const bool index32 = srcMesh.ibFormat == DXGI_FORMAT_R32_UINT;
            if (!index32 && srcMesh.ibFormat != DXGI_FORMAT_R16_UINT)
                return Fail(error, "Unsupported index format");
            const uint32_t indexBytes = index32 ? 4u : 2u;
            const uint32_t indexCount = srcMesh.ibSize / indexBytes;
            const uint32_t vertexCount = srcMesh.vbSize / layout.stride;
            const uint8_t* vb = geometry.data() + srcMesh.vbOffset;
            const uint8_t* ib = geometry.data() + srcMesh.ibOffset;

            // 메시에 대한 스키닝 준비
            const Matrix4& world = nodeWorld[srcMesh.meshCBV];
            const Matrix3 worldIT = InverseTranspose(world.Get3x3());
            std::vector<XMFLOAT4X4> slotMatrices;
            MakeSlotMatrices(world, skeleton + srcMesh.startJoint, palette.numJoints, slotMatrices);
            const uint32_t meshSlot = uint32_t(source.meshes.size());
            bool meshUsed = false;

            std::unordered_map<uint32_t, uint32_t> remap; // source vertex -> patch vertex

            // 정점 복사 람다 정점 하나를 패치에 추가하고 패치 정점 번호를 반환한다.
            const auto addVertex = [&](uint32_t vertex) -> uint32_t
            {
                const auto found = remap.find(vertex);
                if (found != remap.end())
                    return found->second;

                const DecodedVertex v = Decode(vb + size_t(vertex) * layout.stride, layout);

                // Same blend as DefaultVS (weighted joint transforms, mesh world included). The
                // position uses the shared SkinPoint, so later per-frame skinning agrees with it.
                XMFLOAT3 posed;
                XMStoreFloat3(&posed, SkinPoint(slotMatrices.data(), uint32_t(slotMatrices.size()), v.joints, v.weights, 4, v.position));
                XMVECTOR nrm = XMVectorZero();
                const Vector3 bindNrm(v.normal.x, v.normal.y, v.normal.z);
                for (int i = 0; i < 4; ++i)
                {
                    if (v.weights[i] <= 0.0f)
                        continue;
                    const Joint& joint = skeleton[srcMesh.startJoint + v.joints[i]];
                    nrm = XMVectorMultiplyAdd(joint.nrmXform * bindNrm, XMVectorReplicate(v.weights[i]), nrm);
                }
                const Vector3 posedNormal = Normalize(worldIT * Vector3(nrm));

                const float influence = JointSetInfluence(v.joints, v.weights, joints);
                ProceduralVertex out;
                out.position = posed;
                out.normal = { float(posedNormal.GetX()), float(posedNormal.GetY()), float(posedNormal.GetZ()) };
                out.uv = v.uv;
                // Blue at no influence to red at full influence, like the H weight view.
                out.color = { 0.15f + 0.85f * influence, 0.25f * (1.0f - influence), 1.0f - influence, 1.0f };

                BonePatchVertex skin;
                skin.meshSlot = meshSlot;
                skin.sourceVertex = vertex;
                skin.position = v.position;
                skin.normal = v.normal;
                std::memcpy(skin.joints, v.joints, sizeof(skin.joints));
                std::memcpy(skin.weights, v.weights, sizeof(skin.weights));

                const uint32_t patchIndex = uint32_t(mesh.vertices.size());
                mesh.vertices.push_back(out);
                source.vertices.push_back(skin);
                remap.emplace(vertex, patchIndex);
                return patchIndex;
            };

            // 각 삼각형 순회
            for (uint32_t drawIdx = 0; drawIdx < srcMesh.numDraws; ++drawIdx)
            {
                const Mesh::Draw& draw = srcMesh.draw[drawIdx];
                if (draw.primCount % 3 != 0 || draw.startIndex > indexCount
                    || draw.primCount > indexCount - draw.startIndex)
                    return Fail(error, "Draw range exceeds index data");

                for (uint32_t i = 0; i < draw.primCount; i += 3)
                {
                    uint32_t tri[3];
                    for (uint32_t k = 0; k < 3; ++k)
                    {
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
                        tri[k] = draw.baseVertex + index;
                        if (tri[k] >= vertexCount)
                            return Fail(error, "Index refers beyond the vertex buffer");
                    }

                    bool selected = false;
                    bool weightedInSubtree = false;
                    bool weightedOutside = false;
                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        const DecodedVertex v = Decode(vb + size_t(tri[k]) * layout.stride, layout);
                        for (int j = 0; j < 4; ++j)
                        {
                            if (v.weights[j] <= 0.0f)
                                continue;
                            if (v.joints[j] >= srcMesh.numJoints)
                                return Fail(error, "Vertex joint index exceeds the mesh palette");
                            if (inSubtree[v.joints[j]])
                                weightedInSubtree = true;
                            else
                                weightedOutside = true;
                        }
                        selected |= VertexInJointSet(v.joints, v.weights, joints, minWeight);
                    }
                    if (!selected)
                    {
                        if (weightedInSubtree && weightedOutside)
                            ++source.mixedOutsideTriangles;
                        continue;
                    }

                    if (!meshUsed)
                    {
                        // The first mesh that really contributes triangles defines the pose
                        // snapshot: joint matrices live in the mesh's skinning space, so the mesh
                        // world is prepended to put them in the space of the posed patch vertices.
                        // A patch that mixes meshes with different world transforms has no single
                        // such space, so it is rejected.
                        if (source.poseJointWorld.empty())
                        {
                            source.poseJointWorld.reserve(palette.numJoints);
                            for (uint32_t j = 0; j < palette.numJoints; ++j)
                            {
                                const uint32_t jointNode = model.m_JointIndices[palette.startJoint + j];
                                if (jointNode >= model.m_NumNodes)
                                    return Fail(error, "Joint node index out of range");
                                source.poseJointWorld.push_back(world * nodeWorld[jointNode]);
                            }
                            poseWorld = world;
                            havePoseWorld = true;
                        }
                        else if (havePoseWorld && std::memcmp(&poseWorld, &world, sizeof(Matrix4)) != 0)
                            return Fail(error, "Patch spans meshes with different world transforms");
                    }

                    // 삼각형에 대한 정점 추가
                    for (uint32_t k = 0; k < 3; ++k)
                        mesh.indices.push_back(addVertex(tri[k]));
                    source.triangles.push_back({ meshSlot, drawIdx, i / 3 });
                    meshUsed = true;
                }
            }

            if (meshUsed)
                source.meshes.push_back({ meshIndex, srcMesh.meshCBV, srcMesh.startJoint });
        }

        if (mesh.indices.empty())
            return Fail(error, "Selected joint influences no triangles");

        mesh.sections.push_back({ 0, uint32_t(mesh.indices.size()), 0 });
        outMesh = std::move(mesh);
        outSource = std::move(source);
        if (error)
            error->clear();
        return true;
    }

    
    bool ComputePaletteSkeleton(const Model& model, const ModelInstance& instance, uint32_t paletteId,
        SkeletonPose& out, std::string* error)
    {
        // 인스턴스, 포즈, 스킨 팔레트 유효성 검사
        const Matrix4* nodeWorld = instance.GetNodeWorldMatrices();
        if (instance.GetModel() != &model || nodeWorld == nullptr)
            return Fail(error, "Instance has no pose data for this model");
        if (paletteId >= model.m_SkinPalettes.size())
            return Fail(error, "Skin palette out of range");
        const SkinPalette& palette = model.m_SkinPalettes[paletteId];

        // 현재 팔레트에 포함돼있는 스킨 메시들에 대한 월드 행렬 W를 결정
        const Matrix4* meshWorld = nullptr;
        const uint8_t* meshPtr = model.m_MeshData.get();
        for (uint32_t meshIndex = 0; meshIndex < model.m_NumMeshes; ++meshIndex)
        {
            const Mesh& srcMesh = *(const Mesh*)meshPtr;
            meshPtr += sizeof(Mesh) + (srcMesh.numDraws - 1) * sizeof(Mesh::Draw);

            // 입력받은 팔레트를 사용하는 메시인지 확인
            if ((srcMesh.psoFlags & PSOFlags::kHasSkin) == 0 || srcMesh.numJoints == 0 || srcMesh.startJoint != palette.startJoint)
                continue;
            if (srcMesh.meshCBV >= model.m_NumNodes)
                return Fail(error, "Mesh world matrix index out of range");

            if (meshWorld == nullptr)
                meshWorld = &nodeWorld[srcMesh.meshCBV];
            // 같은 팔레트를 사용하지만, 다른 월드 변환 행렬 W를 가질 때
            else if (std::memcmp(meshWorld, &nodeWorld[srcMesh.meshCBV], sizeof(Matrix4)) != 0)
                return Fail(error, "Meshes sharing this palette have different world transforms");
        }
        if (meshWorld == nullptr)
            return Fail(error, "No skinned mesh uses this palette");

        // 각 노드 번호를 조인트 번호로 변환합니다.
        std::unordered_map<uint32_t, uint32_t> nodeToJoint;
        for (uint32_t j = 0; j < palette.numJoints; ++j)
        {
            const uint32_t jointNode = model.m_JointIndices[palette.startJoint + j];
            if (jointNode >= model.m_NumNodes)
                return Fail(error, "Joint node index out of range");
            nodeToJoint.emplace(jointNode, j);
        }

        // 각 joint 마다 위치와 부모 joint를 저장합니다.
        SkeletonPose pose;
        pose.jointPositions.resize(palette.numJoints); // 조인트 수만큼 할당
        pose.parents.assign(palette.numJoints, kNoNode); // 조인트 수만큼 부모 칸 할당
        for (uint32_t j = 0; j < palette.numJoints; ++j)
        {
            // 조인트 번호
            const uint32_t jointNode = model.m_JointIndices[palette.startJoint + j];

            // 월드 행렬을 통한 위치 계산
            const Vector3 position((*meshWorld * nodeWorld[jointNode]).GetW()); 
            pose.jointPositions[j] = { float(position.GetX()), float(position.GetY()), float(position.GetZ()) };

            // 현재 joint의 부모 노드부터 시작해서 위로 올라가며 joint인 부모 노드를 찾는다.
            uint32_t node = jointNode < model.m_NodeParents.size() ? model.m_NodeParents[jointNode] : UINT32_MAX;
            for (uint32_t depth = 0; depth <= model.m_NumNodes && node < model.m_NodeParents.size(); ++depth)
            {
                // 현재 노드가 joint(뼈)이면 부모 joint 탐색 완료
                const auto found = nodeToJoint.find(node);
                if (found != nodeToJoint.end())
                {
                    pose.parents[j] = found->second;
                    break;
                }
                node = model.m_NodeParents[node];
            }
        }
        out = std::move(pose);
        if (error)
            error->clear();
        return true;
    }

    bool MakeWeldInput(const ProceduralMeshData& mesh, const BonePatchSource& source,
        WeldInput& out, std::string* error)
    {
        if (mesh.vertices.size() != source.vertices.size())
            return Fail(error, "Mesh and source disagree on the vertex count");
        if (mesh.indices.size() != source.triangles.size() * 3)
            return Fail(error, "Mesh and source disagree on the triangle count");

        WeldInput in;
        in.vertices.resize(mesh.vertices.size());
        for (size_t i = 0; i < mesh.vertices.size(); ++i)
        {
            const ProceduralVertex& rendered = mesh.vertices[i];
            const BonePatchVertex& skin = source.vertices[i];
            WeldInputVertex& v = in.vertices[i];
            v.meshSlot = skin.meshSlot;
            v.bindPosition = skin.position;
            v.position = rendered.position;
            v.normal = rendered.normal;
            v.uv = rendered.uv;
            v.color = rendered.color;
            for (int k = 0; k < 4; ++k)
            {
                v.joints[k] = skin.joints[k];
                v.weights[k] = skin.weights[k];
            }
        }
        in.indices = mesh.indices;
        out = std::move(in);
        if (error)
            error->clear();
        return true;
    }
}
