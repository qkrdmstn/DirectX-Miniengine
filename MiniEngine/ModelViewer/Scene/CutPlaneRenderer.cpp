#include "CutPlaneRenderer.h"
#include "BufferManager.h"
#include "GraphicsCommon.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "CompiledShaders/CutPlanePickDepthCS.h"
#include "CompiledShaders/CutPlaneVS.h"
#include "CompiledShaders/CutPlanePS.h"
#include <vector>

namespace Viewer
{
    namespace
    {
        struct alignas(16) Vertex
        {
            Math::XMFLOAT3 Position;
            float Padding;
            Math::XMFLOAT4 Color;
        };
        static_assert(sizeof(Vertex) == 32 && alignof(Vertex) == 16, "plane vertex layout");

        // SIMD 벡터를 정렬된 업로드 정점으로 옮긴다.
        void AddVertex(std::vector<Vertex>& vertices, Math::Vector3 position, const Math::XMFLOAT4& color)
        {
            Vertex vertex{};
            DirectX::XMStoreFloat3(&vertex.Position, position);
            vertex.Color = color;
            vertices.push_back(vertex);
        }
    }

    // 직전 프레임의 장면 깊이에서 클릭 픽셀 하나를 읽는다.
    float ReadCutPlanePickDepth(unsigned pixelX, unsigned pixelY, ByteAddressBuffer& sample, ReadbackBuffer& readback)
    {
        using namespace Graphics;

        // 첫 겹침 클릭에서 깊이 조회용 파이프라인과 4바이트 버퍼를 준비한다.
        static RootSignature root;
        static ComputePSO pso(L"Cut Plane Pick Depth");
        if (!sample.GetResource())
        {
            root.Reset(3, 0);
            root[0].InitAsConstants(0, 2);
            root[1].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 1);
            root[2].InitAsDescriptorRange(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 0, 1);
            root.Finalize(L"Cut Plane Pick Depth");
            pso.SetRootSignature(root);
            pso.SetComputeShader(g_pCutPlanePickDepthCS, sizeof(g_pCutPlanePickDepthCS));
            pso.Finalize();
            readback.Create(L"Cut Plane Pick Readback", 1, sizeof(float));
            sample.Create(L"Cut Plane Pick Sample", 1, sizeof(float));
        }

        // 그래픽 큐에서 앞선 프레임의 깊이 쓰기 뒤에 픽셀 조회를 제출한다.
        auto& context = GraphicsContext::Begin(L"Cut Plane Pick Depth");
        auto& compute = context.GetComputeContext();
        context.TransitionResource(g_SceneDepthBuffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        context.TransitionResource(sample, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.SetRootSignature(root);
        compute.SetPipelineState(pso);
        compute.SetConstants(0, pixelX, pixelY);
        compute.SetDynamicDescriptor(1, 0, g_SceneDepthBuffer.GetDepthSRV());
        compute.SetDynamicDescriptor(2, 0, sample.GetUAV());
        compute.Dispatch(1, 1, 1);
        context.CopyBuffer(readback, sample);
        context.Finish(true);

        // GPU 복사가 끝난 뒤 선택에 쓸 깊이 값만 꺼낸다.
        const float depth = *static_cast<const float*>(readback.Map());
        readback.Unmap();
        return depth;
    }

    // 현재 TRS의 네 모서리로 모든 선을 만들어 선택·후속 절단과 같은 기하를 사용한다.
    unsigned RenderCutPlanes(GraphicsContext& context, const CutPlaneSystem& planes,
        ObjectId selected, const Math::Camera& camera, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor)
    {
        std::vector<Vertex> vertices;
        unsigned count = 0;
        for (const auto& plane : planes.GetPlanes())
        {
            MeshCutting::CutPlaneData data;
            if (!planes.BuildData(plane.first, data))
                continue;
            ++count;
            const Math::XMFLOAT4 outline = plane.first == selected ? Math::XMFLOAT4(4, 2.4f, 0.1f, 1)
                : Math::XMFLOAT4(0.1f, 2, 3, 1);
            const Math::XMFLOAT4 grid(0.08f, 0.65f, 0.9f, 1);
            for (int i = 0; i < 4; ++i)
            {
                AddVertex(vertices, data.WorldCorners[i], outline);
                AddVertex(vertices, data.WorldCorners[(i + 1) % 4], outline);
            }
            for (int i = 1; i < 10; ++i)
            {
                const float t = i / 10.0f;
                const auto& c = data.WorldCorners;
                AddVertex(vertices, c[0] + (c[1] - c[0]) * t, grid);
                AddVertex(vertices, c[3] + (c[2] - c[3]) * t, grid);
                AddVertex(vertices, c[0] + (c[3] - c[0]) * t, grid);
                AddVertex(vertices, c[1] + (c[2] - c[1]) * t, grid);
            }
        }
        if (vertices.empty())
            return 0;
        static RootSignature root;
        static GraphicsPSO pso(L"Cut Plane Lines");
        static bool initialized = false;
        if (!initialized)
        {
            root.Reset(1, 0);
            root[0].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
            root.Finalize(L"Cut Plane Lines", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
            const D3D12_INPUT_ELEMENT_DESC layout[] = {
                {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
                {"COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
            };
            pso.SetRootSignature(root);
            pso.SetRasterizerState(Graphics::RasterizerTwoSided);
            pso.SetBlendState(Graphics::BlendDisable);
            pso.SetDepthStencilState(Graphics::DepthStateReadOnly);
            pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
            pso.SetInputLayout(_countof(layout), layout);
            pso.SetRenderTargetFormat(Graphics::g_SceneColorBuffer.GetFormat(), Graphics::g_SceneDepthBuffer.GetFormat());
            pso.SetVertexShader(g_pCutPlaneVS, sizeof(g_pCutPlaneVS));
            pso.SetPixelShader(g_pCutPlanePS, sizeof(g_pCutPlanePS));
            pso.Finalize();
            initialized = true;
        }
        context.TransitionResource(Graphics::g_SceneColorBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET);
        context.TransitionResource(Graphics::g_SceneDepthBuffer, D3D12_RESOURCE_STATE_DEPTH_READ);
        context.SetRenderTarget(Graphics::g_SceneColorBuffer.GetRTV(), Graphics::g_SceneDepthBuffer.GetDSV_DepthReadOnly());
        context.SetViewportAndScissor(viewport, scissor);
        context.SetRootSignature(root);
        context.SetPipelineState(pso);
        const Math::Matrix4 viewProjection = camera.GetViewProjMatrix();
        context.SetDynamicConstantBufferView(0, sizeof(viewProjection), &viewProjection);
        context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        context.SetDynamicVB(0, vertices.size(), sizeof(Vertex), vertices.data());
        context.Draw(static_cast<UINT>(vertices.size()));
        return count;
    }
}
