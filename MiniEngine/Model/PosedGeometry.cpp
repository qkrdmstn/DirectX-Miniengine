#include "PosedGeometry.h"
#include "Model.h"
#include "GeometryBuffers.h"
#include "ConstantBuffers.h"
#include "../Core/CommandContext.h"
#include "../Core/CommandListManager.h"
#include "../Core/GraphicsCore.h"
#include "../Core/PipelineState.h"
#include "../Core/RootSignature.h"
#include "../Core/ReadbackBuffer.h"
#include "../Core/Util/CommandLineArg.h"
#include "../Core/Utility.h"
#include "CompiledShaders/PreparePoseCS.h"
#include <cmath>
#include <cstring>
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace
{
    RootSignature s_PoseRootSignature;
    ComputePSO s_PosePSO(L"Prepare object-local pose");

    // 진단 읽기 매핑을 검사 범위에 묶어 예외가 발생해도 해제한다.
    struct ScopedPoseReadback
    {
        ReadbackBuffer& Buffer;
        const float* Data;

        // 성공한 매핑을 보관하고 이후 범위 종료 시 해제하도록 준비한다.
        explicit ScopedPoseReadback(ReadbackBuffer& buffer) : Buffer(buffer)
        {
            // float 단위 PG 레이아웃으로 읽기 전용 매핑을 연다.
            Data = static_cast<const float*>(Buffer.Map());
        }

        // 정상 종료와 예외 전파 모두에서 읽기 매핑을 닫는다.
        ~ScopedPoseReadback()
        {
            // 범위를 떠날 때 소유한 매핑을 해제한다.
            Buffer.Unmap();
        }
    };

    // 두 방향을 정규화한 뒤 각도 오차를 구하고 비유한 값과 영벡터는 실패로 처리한다.
    double FrameAngleDegrees(const float* actual, const float* expected)
    {
        // 내적과 길이를 double로 누적해 작은 양자화 오차를 측정한다.
        double dot = 0, aa = 0, bb = 0;
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            dot += double(actual[axis]) * expected[axis];
            aa += double(actual[axis]) * actual[axis];
            bb += double(expected[axis]) * expected[axis];
        }

        // 유효한 방향만 정규화하고 acos 입력의 반올림 오차를 제한한다.
        if (!std::isfinite(dot) || !std::isfinite(aa) || !std::isfinite(bb) || aa == 0 || bb == 0)
            return std::numeric_limits<double>::infinity();
        const double cosine = dot / std::sqrt(aa * bb);
        return std::acos(std::max(-1.0, std::min(1.0, cosine))) * (180.0 / 3.14159265358979323846);
    }

    // 최대 오차가 발생한 메시와 로컬 정점 및 탄젠트 유무를 함께 보관한다.
    struct PoseValidationError
    {
        double Error = 0;
        uint32_t MeshIndex = std::numeric_limits<uint32_t>::max();
        uint32_t VertexIndex = std::numeric_limits<uint32_t>::max();
        bool HasTangent = false;

        // 첫 검사 결과 또는 더 큰 오차의 정점 정보를 기록한다.
        void Update(double error, uint32_t meshIndex, uint32_t vertexIndex, bool hasTangent)
        {
            // 최대값이 같으면 최초 정점을 유지하며 첫 결과는 오차가 없어도 기록한다.
            if (MeshIndex == std::numeric_limits<uint32_t>::max() || error > Error)
            {
                Error = error;
                MeshIndex = meshIndex;
                VertexIndex = vertexIndex;
                HasTangent = hasTangent;
            }
        }
    };

    // 생성 PG를 CPU의 Data 디코드와 비교하며 실제 탄젠트가 있는 메시만 T를 검사한다.
    void ValidateGeneratedPose(GraphicsContext& context, ByteAddressBuffer& buffer,
        const std::vector<uint32_t>& offsets, const GeometryBuffers& geometry)
    {
        // 진단 옵션에서만 생성 PG와 packed Data의 동기 readback을 만든다.
        uint32_t enabled = 0;
        if (!CommandLineArgs::GetInteger(L"cut-validate", enabled) || enabled == 0)
            return;
        const uint32_t bytes = static_cast<uint32_t>(buffer.GetBufferSize());
        ReadbackBuffer actualReadback, expectedReadback;
        actualReadback.Create(L"Generated decoded PG readback", bytes, 1);
        auto& reference = const_cast<ByteAddressBuffer&>(geometry.Data);
        expectedReadback.Create(L"Generated reference Data readback", uint32_t(reference.GetBufferSize()), 1);
        context.CopyBuffer(actualReadback, buffer);
        context.CopyBuffer(expectedReadback, reference);
        context.TransitionResource(buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(reference, D3D12_RESOURCE_STATE_GENERIC_READ);
        context.Flush(true);

        // 메시별 출력 구간과 플래그로 최대 오차 및 탄젠트 생략 정점 수를 모은다.
        ScopedPoseReadback actual(actualReadback), expected(expectedReadback);
        PoseValidationError position, normal, tangent;
        uint32_t vertices = 0, tangentSkipped = 0;
        for (uint32_t meshIndex = 0; meshIndex < geometry.Meshes.size(); ++meshIndex)
        {
            // 메시의 로컬 정점 수와 연속 PG 구간의 시작 위치를 구한다.
            const Mesh& mesh = geometry.Meshes[meshIndex];
            const uint32_t count = mesh.vbSize / mesh.vbStride;
            const bool hasTangent = (mesh.psoFlags & PSOFlags::kHasTangent) != 0;
            const size_t firstFloat = offsets[meshIndex] / sizeof(float);
            vertices += count;
            if (!hasTangent)
                tangentSkipped += count;

            // 위치와 법선은 모든 메시의 정점에서 비교한다.
            for (uint32_t vertexIndex = 0; vertexIndex < count; ++vertexIndex)
            {
                // 단위 행렬의 PreparePoseCS와 같은 UNORM 디코드 및 기본 탄젠트를 적용한다.
                const float* a = actual.Data + firstFloat + size_t(vertexIndex) * 10;
                const auto* source = reinterpret_cast<const uint8_t*>(expected.Data)
                    + mesh.vbOffset + size_t(vertexIndex) * mesh.vbStride;
                float b[10] = { 0, 0, 0, 0, 0, 0, 1, 0, 0, 1 };
                std::memcpy(b, source, 12);
                uint32_t packedNormal = 0, packedTangent = 0;
                std::memcpy(&packedNormal, source + 12, 4);
                if (hasTangent)
                    std::memcpy(&packedTangent, source + 16, 4);
                for (uint32_t axis = 0; axis < 3; ++axis)
                {
                    b[3 + axis] = float((packedNormal >> (axis * 10)) & 1023u) / 1023.0f * 2.0f - 1.0f;
                    if (hasTangent)
                        b[6 + axis] = float((packedTangent >> (axis * 10)) & 1023u) / 1023.0f * 2.0f - 1.0f;
                }
                if (hasTangent)
                    b[9] = float(packedTangent >> 30) / 3.0f * 2.0f - 1.0f;

                // 위치 성분과 법선 각도 오차를 해당 정점 정보에 연결한다.
                for (uint32_t axis = 0; axis < 3; ++axis)
                {
                    const double error = std::abs(double(a[axis]) - b[axis]);
                    position.Update(std::isfinite(error) ? error : std::numeric_limits<double>::infinity(),
                        meshIndex, vertexIndex, hasTangent);
                }
                normal.Update(FrameAngleDegrees(a + 3, b + 3), meshIndex, vertexIndex, hasTangent);

                // 탄젠트가 있는 메시에서만 방향과 w 부호를 검사하고 불일치는 실패로 기록한다.
                if (hasTangent)
                {
                    const bool sameSign = std::isfinite(a[9]) && std::isfinite(b[9])
                        && std::signbit(a[9]) == std::signbit(b[9]);
                    tangent.Update(sameSign ? FrameAngleDegrees(a + 6, b + 6)
                        : std::numeric_limits<double>::infinity(), meshIndex, vertexIndex, hasTangent);
                }
            }
        }

        // 기존 허용치를 적용하고 각 최대 오차의 메시와 로컬 정점 및 탄젠트 유무를 출력한다.
        const bool valid = position.Error <= 1e-6 && normal.Error <= 0.5 && tangent.Error <= 0.5;
        Utility::Printf("Generated PG decode %s: vertices=%u maxPosError=%.9g maxNormalDeg=%.9g maxTangentDeg=%.9g tangentSkipped=%u\n",
            valid ? "PASS" : "FAIL", vertices, position.Error, normal.Error, tangent.Error, tangentSkipped);

        // Printf의 256바이트 버퍼를 넘지 않도록 최대 오차 위치는 다음 줄에 따로 출력한다.
        Utility::Printf("Generated PG decode worst: pos(mesh=%u vertex=%u tan=%u) normal(mesh=%u vertex=%u tan=%u) tangent(mesh=%u vertex=%u tan=%u)\n",
            position.MeshIndex, position.VertexIndex, uint32_t(position.HasTangent),
            normal.MeshIndex, normal.VertexIndex, uint32_t(normal.HasTangent),
            tangent.MeshIndex, tangent.VertexIndex, uint32_t(tangent.HasTangent));
        if (!valid)
            throw std::runtime_error("Generated PG decode validation failed");
    }

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

// 제출된 이전 프레임이 읽는 버퍼를 queue fence 완료까지 지연 해제한다.
PosedGeometry::~PosedGeometry()
{
    // GPU resource의 강한 참조를 manager에 넘기고 wrapper를 즉시 정리한다.
    if (m_Buffer.GetResource() != nullptr)
        Graphics::g_CommandManager.RetireResource(m_Buffer.GetResource());
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

// 생성 Data의 메시별 정점 수로 연속된 40바이트 출력 구간을 할당한다.
PosedGeometry::PosedGeometry(const GeometryBuffers& geometry)
{
    // Data 범위와 생성 정점 계약을 검사하며 메시 순서대로 출력 offset을 누적한다.
    uint64_t size = 0;
    for (const Mesh& mesh : geometry.Meshes)
    {
        if (mesh.vbStride != 28 || mesh.vbSize % 28 != 0 || (mesh.psoFlags & PSOFlags::kHasSkin)
            || uint64_t(mesh.vbOffset) + mesh.vbSize > geometry.Data.GetBufferSize())
            throw std::runtime_error("Unsupported generated pose input");
        m_Offsets.push_back(static_cast<uint32_t>(size));
        size += uint64_t(mesh.vbSize / mesh.vbStride) * VertexStride;
        if (size > std::numeric_limits<uint32_t>::max())
            throw std::runtime_error("Generated posed geometry exceeds 4 GiB");
    }

    // Data의 정점 수로 계산한 연속 PG 공간만 만든다.
    if (size > 0)
        m_Buffer.Create(L"Generated component pose", static_cast<uint32_t>(size), 1);
}

// 이미 component-local인 Data를 단위 행렬로 한 번 디코드하고 이후 결과를 재사용한다.
void PosedGeometry::UpdateGenerated(GraphicsContext& context, const GeometryBuffers& geometry)
{
    // 준비된 결과와 빈 출력에는 dispatch를 기록하지 않는다.
    if (m_Valid || !m_Buffer.GetBufferSize())
        return;

    // 기존 디코드 파이프라인에 skin 없는 입력과 단위 위치 및 법선 행렬을 바인딩한다.
    InitializePosePipeline();
    auto& compute = context.GetComputeContext();
    auto& source = const_cast<ByteAddressBuffer&>(geometry.Data);
    compute.SetRootSignature(s_PoseRootSignature);
    compute.SetPipelineState(s_PosePSO);
    compute.TransitionResource(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    compute.TransitionResource(m_Buffer, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    compute.SetBufferSRV(1, source);
    compute.SetBufferSRV(2, source);
    MeshConstants identity{};
    identity.World = Math::Matrix4(Math::kIdentity);
    identity.WorldIT = Math::Matrix3(Math::kIdentity);
    compute.SetDynamicConstantBufferView(4, sizeof(identity), &identity);

    // 메시별 Data 구간을 연속 PG 구간에 기록하며 N/T는 셰이더의 원래 규칙을 따른다.
    for (size_t i = 0; i < geometry.Meshes.size(); ++i)
    {
        const Mesh& mesh = geometry.Meshes[i];
        const uint32_t count = mesh.vbSize / mesh.vbStride;
        if (count == 0)
            continue;
        const uint32_t constants[] = { mesh.vbOffset, mesh.vbStride, count, mesh.psoFlags };
        compute.SetConstantArray(0, 4, constants);
        compute.SetBufferUAV(3, m_Buffer, m_Offsets[i]);
        const uint32_t groups = (count + 63) / 64;
        compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
    }

    // 렌더용 읽기 상태를 복원하고 옵션이 켜진 첫 갱신에서만 CPU 디코드와 비교한다.
    compute.TransitionResource(m_Buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    compute.TransitionResource(source, D3D12_RESOURCE_STATE_GENERIC_READ);
    ValidateGeneratedPose(context, m_Buffer, m_Offsets, geometry);
    m_Valid = true;
}
