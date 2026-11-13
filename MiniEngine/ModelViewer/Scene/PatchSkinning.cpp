#include "PatchSkinning.h"
#include "GeometryRevision.h"
#include "CommandContext.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "CompiledShaders/PatchSkinBuildCS.h"
#include <algorithm>

namespace Viewer
{
    // 패치 출처와 원본 draw를 묶어 GPU에서 bind 속성을 복원한다.
    bool BuildPatchSkinVertices(const GeometryRevision& source, ByteAddressBuffer& triangleSources,
        const GeometryBuffers& piece, const PatchSkinSnapshot& snapshot, PatchSkinVertices& out)
    {
        // u16 강체 슬롯과 최초 원본 revision을 요구한다.
        if (!snapshot.SkinningAllowed || source.IsGenerated() || snapshot.JointsAtCut.empty()
            || snapshot.JointsAtCut.size() > 65535 || !triangleSources.GetBufferSize())
            return false;
        struct DrawInput
        {
            uint32_t Section, Draw, VbOffset, SourceStride;
            uint32_t IbOffset, Index32, StartIndex, BaseVertex;
            uint32_t SourceFlags, StartJoint;
        };
        struct Parameters
        {
            Math::Matrix4 InverseNode;
            uint32_t FirstVertex, VertexCount, Flags, Stride;
            uint32_t DrawCount, RigidJoint, Padding[2];
        };
        static_assert(sizeof(DrawInput) == 40 && sizeof(Parameters) == 96, "Patch skin shader layout");
        std::vector<DrawInput> draws;
        uint32_t flags = 259;
        for (uint32_t s = 0; s < source.GetSections().size(); ++s)
        {
            const auto& mesh = source.GetMesh(s);
            flags |= mesh.psoFlags & 28;
            if (uint64_t(mesh.startJoint) + mesh.numJoints > snapshot.JointsAtCut.size())
                return false;
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                const auto& draw = mesh.draw[d];
                draws.push_back({ s, d, mesh.vbOffset, mesh.vbStride, mesh.ibOffset,
                    mesh.ibFormat == DXGI_FORMAT_R32_UINT, draw.startIndex, draw.baseVertex,
                    mesh.psoFlags, mesh.startJoint });
            }
        }

        // cutter의 빈 capacity 구간은 건너뛰고 실제 게시 VB 범위만 생성한다.
        uint32_t end = 0;
        for (const auto& mesh : piece.Meshes)
        {
            if (mesh.vbStride != 28 || mesh.vbOffset % 28 || mesh.vbSize % 28
                || (mesh.psoFlags & 31) != 31
                || uint64_t(mesh.vbOffset) + mesh.vbSize > piece.Data.GetBufferSize())
                return false;
            end = std::max(end, mesh.vbOffset / 28 + mesh.vbSize / 28);
        }
        const uint32_t stride = 32 + ((flags & 4) ? 4 : 0) + ((flags & 8) ? 4 : 0) + ((flags & 16) ? 4 : 0);
        if (!end || draws.empty() || uint64_t(end) * stride > UINT32_MAX
            || uint64_t(end) * 32 > piece.Provenance.GetBufferSize())
            return false;

        // 추출 셰이더와 같은 root descriptor 방식으로 compute 파이프라인을 재사용한다.
        static RootSignature root;
        static ComputePSO pso(L"Build patch skin vertices");
        static bool initialized = false;
        if (!initialized)
        {
            root.Reset(7);
            root[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 5; ++i)
                root[i + 1].InitAsBufferSRV(i);
            root[6].InitAsBufferUAV(0);
            root.Finalize(L"Patch skin build");
            pso.SetRootSignature(root);
            pso.SetComputeShader(g_pPatchSkinBuildCS, sizeof(g_pPatchSkinBuildCS));
            pso.Finalize();
            initialized = true;
        }

        // section flags의 합집합을 단일 출력 레이아웃으로 보관한다.
        StructuredBuffer descriptors;
        descriptors.Create(L"Patch skin draws", uint32_t(draws.size()), sizeof(DrawInput), draws.data());
        out.Data.Create(L"Patch skin vertices", end, stride);
        out.Flags = flags;
        out.Stride = stride;
        auto& graphics = GraphicsContext::Begin(L"Build patch skin vertices");
        auto& compute = graphics.GetComputeContext();
        compute.SetRootSignature(root);
        compute.SetPipelineState(pso);
        GpuBuffer* inputs[] = { &const_cast<ByteAddressBuffer&>(source.GetDataBuffer()), &triangleSources,
            &const_cast<ByteAddressBuffer&>(piece.Data), &const_cast<ByteAddressBuffer&>(piece.Provenance), &descriptors };
        for (uint32_t i = 0; i < 5; ++i)
        {
            compute.TransitionResource(*inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            compute.SetBufferSRV(i + 1, *inputs[i]);
        }
        compute.TransitionResource(out.Data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.SetBufferUAV(6, out.Data);
        Parameters c{ Math::Invert(snapshot.MeshNodeAtCut), 0, 0, flags, stride,
            uint32_t(draws.size()), uint32_t(snapshot.JointsAtCut.size()), {} };
        for (const auto& mesh : piece.Meshes)
        {
            c.FirstVertex = mesh.vbOffset / 28;
            c.VertexCount = mesh.vbSize / 28;
            if (!c.VertexCount)
                continue;
            compute.SetDynamicConstantBufferView(0, sizeof(c), &c);
            const uint32_t groups = (c.VertexCount + 63) / 64;
            compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
        }

        // 원본·조각 Data는 이후 VB/IB로도 묶이므로 다른 절단 작업처럼 GENERIC_READ로 되돌린다.
        for (uint32_t i = 0; i < 5; ++i)
            graphics.TransitionResource(*inputs[i], D3D12_RESOURCE_STATE_GENERIC_READ);
        graphics.TransitionResource(out.Data, D3D12_RESOURCE_STATE_GENERIC_READ);

        // 호출자가 보관한 원본·출처·조각과 임시 descriptor는 완료 뒤에만 해제할 수 있다.
        graphics.Finish(true);
        return true;
    }

    // 절단 시점의 고정 본을 현재 부착 프레임으로 옮기며 normal 행렬도 다시 계산한다.
    void BuildPatchPalette(const PatchSkinSnapshot& snapshot, const Joint* currentJoints,
        const Math::Matrix4* currentNodes, std::vector<Joint>& out)
    {
        // 전체 owner 팔레트를 유지하고 부착 노드의 모델 공간 변화량을 구한다.
        const size_t count = snapshot.JointsAtCut.size();
        out.assign(currentJoints, currentJoints + count);
        const Math::Matrix4 delta = currentNodes[snapshot.AttachNode] * Math::Invert(snapshot.AttachAtCut);

        // 고정 본만 절단 pose로 치환하고 최종 위치 행렬의 역전치로 법선을 만든다.
        for (size_t k = 0; k < count; ++k)
        {
            if (!snapshot.Frozen[k]) continue;
            out[k].posXform = delta * snapshot.JointsAtCut[k].posXform;
            out[k].nrmXform = Math::InverseTranspose(out[k].posXform.Get3x3());
        }

        // 새 경계와 cap 정점이 사용할 단일 강체 슬롯을 마지막에 추가한다.
        Joint rigid;
        rigid.posXform = delta;
        rigid.nrmXform = Math::InverseTranspose(delta.Get3x3());
        out.push_back(rigid);
    }
}
