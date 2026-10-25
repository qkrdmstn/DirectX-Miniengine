#include "PosedGeometry.h"
#include "Model.h"
#include "GeometryBuffers.h"
#include "ConstantBuffers.h"
#include "../Core/CommandContext.h"
#include "../Core/CommandListManager.h"
#include "../Core/GraphicsCore.h"
#include "../Core/PipelineState.h"
#include "../Core/RootSignature.h"
#include "CompiledShaders/PreparePoseCS.h"
#include <cstring>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace
{
    RootSignature s_PoseRootSignature;
    ComputePSO s_PosePSO(L"Prepare object-local pose");

    // 원본 RAW SRV와 본 팔레트에서 객체별 RAW UAV를 만드는 파이프라인을 준비한다.
    void InitializePosePipeline()
    {
        static bool initialized = false;
        if (initialized)
            return;
        s_PoseRootSignature.Reset(5);
        s_PoseRootSignature[0].InitAsConstants(0, 4);
        s_PoseRootSignature[1].InitAsBufferSRV(0);
        s_PoseRootSignature[2].InitAsBufferSRV(1);
        s_PoseRootSignature[3].InitAsBufferUAV(0);
        s_PoseRootSignature[4].InitAsConstantBuffer(1);
        s_PoseRootSignature.Finalize(L"Prepare pose");
        s_PosePSO.SetRootSignature(s_PoseRootSignature);
        s_PosePSO.SetComputeShader(g_pPreparePoseCS, sizeof(g_pPreparePoseCS));
        s_PosePSO.Finalize();
        initialized = true;
    }
}

// 원본 mesh별 정점 수와 레이아웃을 검증하고 연속 출력 구간을 할당한다.
PosedGeometry::PosedGeometry(const Model& model)
{
    uint64_t size = 0;
    const uint8_t* meshData = model.m_MeshData.get();
    for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
    {
        const Mesh& mesh = *reinterpret_cast<const Mesh*>(meshData);
        const uint32_t expectedStride = 16 + ((mesh.psoFlags & PSOFlags::kHasTangent) ? 4 : 0)
            + ((mesh.psoFlags & PSOFlags::kHasUV0) ? 4 : 0)
            + ((mesh.psoFlags & PSOFlags::kHasUV1) ? 4 : 0)
            + ((mesh.psoFlags & PSOFlags::kHasSkin) ? 16 : 0);
        const uint32_t depthStride = 12 + ((mesh.psoFlags & PSOFlags::kAlphaTest) ? 4 : 0)
            + ((mesh.psoFlags & PSOFlags::kHasSkin) ? 16 : 0);
        if (mesh.vbStride != expectedStride || mesh.vbSize % mesh.vbStride != 0
            || uint64_t(mesh.vbOffset) + mesh.vbSize > model.m_DataBuffer.GetBufferSize()
            || mesh.vbDepthSize / depthStride != mesh.vbSize / mesh.vbStride
            || mesh.vbDepthSize % depthStride != 0
            || mesh.meshCBV >= model.m_NumNodes
            || (mesh.numJoints > 0 && uint32_t(mesh.startJoint) + mesh.numJoints > model.m_NumJoints)
            || ((mesh.psoFlags & PSOFlags::kHasSkin) && mesh.numJoints == 0))
            throw std::runtime_error("Unsupported posed geometry input");
        m_Offsets.push_back(static_cast<uint32_t>(size));
        size += uint64_t(mesh.vbSize / mesh.vbStride) * VertexStride;
        if (size > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Posed geometry exceeds 4 GiB");
        meshData += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
    }
    if (size > 0)
        m_Buffer.Create(L"Instance posed vertices", static_cast<uint32_t>(size), 1);
}

// 삭제와 모델 교체 시 이전 프레임이 읽는 자원을 GPU 완료 전 해제하지 않는다.
PosedGeometry::~PosedGeometry()
{
    // 첫 경로는 전역 완료를 기다리며, 지연 해제 큐 최적화는 후속 단계에서 다룬다.
    if (m_Buffer.GetBufferSize() > 0)
        Graphics::g_CommandManager.IdleGPU();
}

// 스키닝과 node 변환을 같은 graphics queue에 기록하고 결과를 SRV 상태로 넘긴다.
void PosedGeometry::Update(GraphicsContext& context, const Model& model,
    const Math::Matrix4* nodeTransforms, const Joint* joints)
{
    if (m_Buffer.GetBufferSize() == 0)
        return;
    if (m_Valid && model.m_NumJoints == 0
        && std::memcmp(m_LastNodeTransforms.data(), nodeTransforms,
            model.m_NumNodes * sizeof(Math::Matrix4)) == 0)
        return;

    InitializePosePipeline();
    auto& compute = context.GetComputeContext();
    compute.SetRootSignature(s_PoseRootSignature);
    compute.SetPipelineState(s_PosePSO);
    compute.TransitionResource(m_Buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    compute.SetBufferSRV(1, model.m_DataBuffer);
    const uint8_t* meshData = model.m_MeshData.get();
    for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
    {
        const Mesh& mesh = *reinterpret_cast<const Mesh*>(meshData);
        const uint32_t count = mesh.vbSize / mesh.vbStride;
        const uint32_t constants[] = { mesh.vbOffset, mesh.vbStride, count, mesh.psoFlags };
        compute.SetConstantArray(0, 4, constants);
        if (mesh.numJoints > 0)
            compute.SetDynamicSRV(2, sizeof(Joint) * mesh.numJoints, joints + mesh.startJoint);
        else
            compute.SetBufferSRV(2, model.m_DataBuffer);
        compute.SetBufferUAV(3, m_Buffer, m_Offsets[i]);
        MeshConstants nodeConstants{};
        nodeConstants.World = nodeTransforms[mesh.meshCBV];
        nodeConstants.WorldIT = Math::InverseTranspose(nodeConstants.World.Get3x3());
        compute.SetDynamicConstantBufferView(4, sizeof(nodeConstants), &nodeConstants);
        const uint32_t groups = (count + 63) / 64;
        if (groups > 0)
            compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
        meshData += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
    }
    compute.TransitionResource(m_Buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_LastNodeTransforms.assign(nodeTransforms, nodeTransforms + model.m_NumNodes);
    m_Valid = true;
}

// 생성 geometry의 원본 구간을 유지하는 전용 출력 cache를 할당한다.
PosedGeometry::PosedGeometry(const GeometryBuffers& geometry)
    : m_Offsets(geometry.PoseOffsets)
{
    if (geometry.Pose.GetBufferSize())
        m_Buffer.Create(L"Generated component pose", static_cast<uint32_t>(geometry.Pose.GetBufferSize()), 1);
}

// GPU-to-GPU 복사로 baked geometry를 준비하며 원본 node 변환을 반복하지 않는다.
void PosedGeometry::UpdateBaked(GraphicsContext& context, const GeometryBuffers& geometry)
{
    if (m_Valid || !m_Buffer.GetBufferSize())
        return;
    context.CopyBuffer(m_Buffer, const_cast<ByteAddressBuffer&>(geometry.Pose));
    context.TransitionResource(m_Buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    context.TransitionResource(const_cast<ByteAddressBuffer&>(geometry.Pose), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    m_Valid = true;
}
