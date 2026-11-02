#include "SkeletalPatchCopy.h"
#include "CommandContext.h"
#include "CommandListManager.h"
#include "PipelineState.h"
#include "RootSignature.h"
#include "Renderer.h"
#include "CompiledShaders/PatchJointVoteCS.h"
#include "CompiledShaders/PatchJointSelectCS.h"
#include <cstring>

namespace Viewer
{
    // 완료 전 작업 자원을 파괴하지 않도록 제출 fence를 기다린다.
    SkeletalPatchJob::~SkeletalPatchJob()
    {
        // 제출되지 않은 작업은 GPU 대기가 필요 없다.
        if (m_Fence != 0)
            Graphics::g_CommandManager.WaitForFence(m_Fence);
    }

    // 현재 posed 입력의 모든 skin section draw에 투표와 선택 패스를 기록한다.
    bool SkeletalPatchJob::Submit(const MeshComponent& source, const MeshCutting::CutPlaneData& plane)
    {
        // 유효한 compute pose와 원본 skin 입력만 단일 제출로 받는다.
        const auto& input = source.GetGeometry();
        const auto& posed = source.GetPosedGeometry();
        const auto& model = *input->GetSource();
        if (m_Fence || !Renderer::UseComputePose || input->IsGenerated() || !posed.IsReady()
            || !model.m_NumJoints || !model.m_NumNodes)
            return false;

        // RAW 주소와 교차 수가 32비트 범위를 넘거나 draw가 버퍼 밖을 읽는 입력은 거절한다.
        if (model.m_NumNodes > (UINT32_MAX - 4ull) / 8)
            return false;
        uint64_t triangles = 0;
        for (size_t i = 0; i < input->GetSections().size(); ++i)
        {
            const auto& mesh = input->GetMesh(i);
            if (!mesh.numJoints)
                continue;
            if (mesh.vbStride < 16 || mesh.vbSize % mesh.vbStride
                || uint64_t(mesh.vbOffset) + mesh.vbSize > (std::min)(input->GetDataBuffer().GetBufferSize(), size_t(UINT32_MAX))
                || (mesh.ibFormat != DXGI_FORMAT_R16_UINT && mesh.ibFormat != DXGI_FORMAT_R32_UINT)
                || mesh.ibOffset % 4 || uint64_t(mesh.ibOffset) + mesh.ibSize > (std::min)(input->GetDataBuffer().GetBufferSize(), size_t(UINT32_MAX))
                || uint64_t(mesh.startJoint) + mesh.numJoints > model.m_NumJoints)
                return false;
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                const auto& draw = mesh.draw[d];
                const uint32_t stride = mesh.ibFormat == DXGI_FORMAT_R32_UINT ? 4 : 2;
                if (draw.primCount % 3 || (uint64_t(draw.startIndex) + draw.primCount) * stride > mesh.ibSize)
                    return false;
                triangles += draw.primCount / 3;
            }
        }
        if (triangles > UINT32_MAX)
            return false;

        // 기존 CutJob 방식의 root buffer binding과 컴파일된 compute PSO를 준비한다.
        static RootSignature root;
        static ComputePSO vote, select;
        static bool ready = false;
        if (!ready)
        {
            root.Reset(6);
            root[0].InitAsConstantBuffer(0);
            for (uint32_t i = 0; i < 3; ++i)
                root[i + 1].InitAsBufferSRV(i);
            for (uint32_t i = 0; i < 2; ++i)
                root[i + 4].InitAsBufferUAV(i);
            root.Finalize(L"Skeletal patch selection");
            vote.SetRootSignature(root);
            vote.SetComputeShader(g_pPatchJointVoteCS, sizeof(g_pPatchJointVoteCS));
            vote.Finalize();
            select.SetRootSignature(root);
            select.SetComputeShader(g_pPatchJointSelectCS, sizeof(g_pPatchJointSelectCS));
            select.Finalize();
            ready = true;
        }

        // 업로드의 16바이트 SIMD 읽기가 배열 끝을 넘지 않도록 palette 표에 패딩을 둔다.
        std::vector<uint32_t> nodes((size_t(model.m_NumJoints) + 3) & ~size_t(3));
        for (uint32_t i = 0; i < model.m_NumJoints; ++i)
            nodes[i] = model.m_JointIndices[i];
        m_Input = input;
        m_JointNodes.Create(L"Patch joint nodes", model.m_NumJoints, 4, nodes.data());
        m_Votes.Create(L"Patch votes", model.m_NumNodes * 2 + 1, 4);
        m_Result.Create(L"Patch result", 3, 4);
        m_Readback.Create(L"Patch readback", 3, 4);
        struct Constants
        {
            Math::Vector4 Plane;
            Math::Matrix4 LocalToPlane;
            float HalfWidth, HalfHeight, Epsilon;
            uint32_t Nodes, VertexOffset, VertexStride, IndexOffset, Index32, StartIndex;
            int32_t BaseVertex;
            uint32_t Triangles, PoseOffset, StartJoint, VertexCount, JointCount;
        } constants{};
        constants.Plane = plane.WorldPlane;
        constants.LocalToPlane = plane.WorldToPlane;
        constants.HalfWidth = plane.HalfWidth;
        constants.HalfHeight = plane.HalfHeight;
        constants.Epsilon = 1e-5f;
        constants.Nodes = model.m_NumNodes;

        // pose 갱신과 같은 direct queue의 compute context로 읽기 순서를 유지한다.
        auto& graphics = GraphicsContext::Begin(L"Skeletal patch selection");
        auto& compute = graphics.GetComputeContext();
        compute.SetRootSignature(root);
        compute.TransitionResource(m_Votes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.TransitionResource(m_Result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.ClearUAV(m_Votes);
        compute.InsertUAVBarrier(m_Votes);
        compute.SetBufferSRV(1, input->GetDataBuffer());
        compute.SetBufferSRV(2, posed.GetBuffer());
        compute.SetBufferSRV(3, m_JointNodes);
        compute.SetBufferUAV(4, m_Votes);
        compute.SetBufferUAV(5, m_Result);
        compute.SetPipelineState(vote);
        for (size_t i = 0; i < input->GetSections().size(); ++i)
        {
            const auto& mesh = input->GetMesh(i);
            if (!mesh.numJoints)
                continue;
            constants.VertexOffset = mesh.vbOffset;
            constants.VertexStride = mesh.vbStride;
            constants.IndexOffset = mesh.ibOffset;
            constants.Index32 = mesh.ibFormat == DXGI_FORMAT_R32_UINT;
            constants.PoseOffset = posed.GetOffset(input->GetSections()[i].SourceMesh);
            constants.StartJoint = mesh.startJoint;
            constants.VertexCount = mesh.vbSize / mesh.vbStride;
            constants.JointCount = mesh.numJoints;
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                constants.StartIndex = mesh.draw[d].startIndex;
                constants.BaseVertex = mesh.draw[d].baseVertex;
                constants.Triangles = mesh.draw[d].primCount / 3;
                compute.SetDynamicConstantBufferView(0, sizeof(constants), &constants);
                const uint32_t groups = (constants.Triangles + 63) / 64;
                compute.Dispatch((std::min)(groups, 65535u), (groups + 65534) / 65535);
            }
        }

        // 투표가 완료된 뒤 단일 그룹으로 선택하고 작은 결과만 복사한다.
        compute.InsertUAVBarrier(m_Votes);
        compute.SetDynamicConstantBufferView(0, sizeof(constants), &constants);
        compute.SetPipelineState(select);
        compute.Dispatch(1);
        graphics.CopyBuffer(m_Readback, m_Result);
        m_Fence = graphics.Finish();
        return true;
    }

    // fence 완료 여부만 확인하고 대기 없이 작은 결과를 수집한다.
    bool SkeletalPatchJob::Poll(SkeletalPatchResult& result, std::shared_ptr<GeometryBuffers>& patch)
    {
        // GPU가 아직 쓰는 readback은 매핑하지 않는다.
        if (!m_Fence || !Graphics::g_CommandManager.IsFenceComplete(m_Fence))
            return false;

        // SP-2에는 추출 버퍼가 없으며 세 DWORD만 반환한다.
        std::memcpy(&result, m_Readback.Map(), sizeof(result));
        m_Readback.Unmap();
        patch.reset();
        return true;
    }
}
