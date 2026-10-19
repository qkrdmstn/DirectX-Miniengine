#include "../pch.h"
#include "ProceduralMeshRenderer.h"
#include "../../Core/CommandContext.h"
#include "../../Core/CommandListManager.h"
#include "../../Core/GraphicsCommon.h"
#include "../../Core/ColorBuffer.h"
#include "../../Core/DepthBuffer.h"
#include <algorithm>
#include <cstring>
#include <malloc.h>
#include <stdexcept>

#include "CompiledShaders/ProceduralMeshVS.h"
#include "CompiledShaders/ProceduralMeshPS.h"

using namespace Graphics;

namespace MeshCutting
{
    ProceduralMeshRenderer::ProceduralMeshRenderer() :
        m_Pso(L"Procedural Mesh Color")
    {
    }

    ProceduralMeshRenderer::~ProceduralMeshRenderer()
    {
        Shutdown();
    }

    void ProceduralMeshRenderer::Initialize()
    {
        if (m_Initialized)
            return;

        m_RootSignature.Reset(1, 0);
        m_RootSignature[0].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
        m_RootSignature.Finalize(L"Procedural Mesh Root", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        static const D3D12_INPUT_ELEMENT_DESC layout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(ProceduralVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(ProceduralVertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(ProceduralVertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(ProceduralVertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        static_assert(sizeof(ProceduralVertex) == 48, "Procedural vertex layout changed");

        m_Pso.SetRootSignature(m_RootSignature);
        m_Pso.SetRasterizerState(RasterizerTwoSided);
        m_Pso.SetBlendState(BlendDisable);
        m_Pso.SetDepthStencilState(DepthStateReadWrite);
        m_Pso.SetInputLayout(_countof(layout), layout);
        m_Pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        m_Pso.SetVertexShader(g_pProceduralMeshVS, sizeof(g_pProceduralMeshVS));
        m_Pso.SetPixelShader(g_pProceduralMeshPS, sizeof(g_pProceduralMeshPS));
        m_Initialized = true;
    }

    ProceduralMeshRenderer::BufferPtr ProceduralMeshRenderer::Acquire(uint32_t bytes)
    {
        Collect();
        const uint64_t required64 = (uint64_t(bytes) + 15u) & ~uint64_t(15u);
        if (required64 > UINT32_MAX)
            throw std::length_error("Procedural GPU buffer exceeds D3D12 byte size");
        const uint32_t required = uint32_t(required64);
        auto candidate = std::find_if(m_Free.begin(), m_Free.end(), [required](const BufferPtr& buffer)
        {
            return buffer->capacityBytes >= required;
        });
        if (candidate != m_Free.end())
        {
            BufferPtr buffer = *candidate;
            m_Free.erase(candidate);
            return buffer;
        }

        auto buffer = std::make_shared<Buffer>();
        buffer->capacityBytes = required;
        buffer->resource.Create(L"Procedural Mesh Buffer", required / 4u, 4u);
        return buffer;
    }

    void ProceduralMeshRenderer::Collect()
    {
        auto it = m_Retired.begin();
        while (it != m_Retired.end())
        {
            const BufferPtr& buffer = *it;
            if (!buffer->pending && (buffer->fence == 0 || g_CommandManager.IsFenceComplete(buffer->fence)))
            {
                m_Free.push_back(buffer);
                it = m_Retired.erase(it);
            }
            else
                ++it;
        }
    }

    void ProceduralMeshRenderer::Retire(BufferPtr buffer)
    {
        if (buffer)
            m_Retired.push_back(std::move(buffer));
    }

    void ProceduralMeshRenderer::Upload(GraphicsContext& context, Buffer& buffer, const void* data, uint32_t bytes)
    {
        const size_t padded = (size_t(bytes) + 15u) & ~size_t(15u);
        void* staging = _aligned_malloc(padded, 16);
        if (!staging)
            throw std::bad_alloc();
        std::memset(staging, 0, padded);
        std::memcpy(staging, data, bytes);
        context.WriteBuffer(buffer.resource, 0, staging, padded);
        _aligned_free(staging);
    }

    void ProceduralMeshRenderer::Draw(GraphicsContext& context, const ProceduralMesh& mesh,
        const Math::Matrix4& world, const Math::Matrix4& viewProj,
        ColorBuffer& color, DepthBuffer& depth,
        const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor)
    {
        Initialize();
        if (!m_PsoFinalized || m_ColorFormat != color.GetFormat() || m_DepthFormat != depth.GetFormat())
        {
            m_ColorFormat = color.GetFormat();
            m_DepthFormat = depth.GetFormat();
            m_Pso.SetRenderTargetFormat(m_ColorFormat, m_DepthFormat);
            m_Pso.Finalize();
            m_PsoFinalized = true;
        }
        const ProceduralMeshData& data = mesh.GetData();
        MeshGpu& gpu = m_Meshes[&mesh];
        if (data.vertices.empty() || data.indices.empty())
        {
            Retire(std::move(gpu.vertices));
            Retire(std::move(gpu.indices));
            gpu.vertexVersion = mesh.GetVertexVersion();
            gpu.topologyVersion = mesh.GetTopologyVersion();
            return;
        }

        const uint32_t vertexBytes = uint32_t(data.vertices.size() * sizeof(ProceduralVertex));
        const uint32_t indexBytes = uint32_t(data.indices.size() * sizeof(uint32_t));
        if (!gpu.vertices || gpu.vertexVersion != mesh.GetVertexVersion())
        {
            BufferPtr replacement = Acquire(vertexBytes);
            Upload(context, *replacement, data.vertices.data(), vertexBytes);
            Retire(std::move(gpu.vertices));
            gpu.vertices = std::move(replacement);
            gpu.vertexVersion = mesh.GetVertexVersion();
        }
        if (!gpu.indices || gpu.topologyVersion != mesh.GetTopologyVersion())
        {
            BufferPtr replacement = Acquire(indexBytes);
            Upload(context, *replacement, data.indices.data(), indexBytes);
            Retire(std::move(gpu.indices));
            gpu.indices = std::move(replacement);
            gpu.topologyVersion = mesh.GetTopologyVersion();
        }

        context.TransitionResource(gpu.vertices->resource, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        context.TransitionResource(gpu.indices->resource, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        context.TransitionResource(color, D3D12_RESOURCE_STATE_RENDER_TARGET);
        context.TransitionResource(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
        context.SetRenderTarget(color.GetRTV(), depth.GetDSV());
        context.SetViewportAndScissor(viewport, scissor);
        context.SetRootSignature(m_RootSignature);
        context.SetPipelineState(m_Pso);
        context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        struct alignas(16) Transforms { Math::Matrix4 world; Math::Matrix4 viewProj; };
        const Transforms transforms{ world, viewProj };
        context.SetDynamicConstantBufferView(0, sizeof(transforms), &transforms);
        context.SetVertexBuffer(0, gpu.vertices->resource.VertexBufferView(0, vertexBytes, sizeof(ProceduralVertex)));
        context.SetIndexBuffer(gpu.indices->resource.IndexBufferView(0, indexBytes, true));
        const auto drawSection = [&context](const MeshSection& section)
        {
            if (section.indexCount != 0)
                context.DrawIndexed(section.indexCount, section.firstIndex);
        };
        if (data.sections.empty())
            context.DrawIndexed(uint32_t(data.indices.size()));
        else
            for (const MeshSection& section : data.sections)
                drawSection(section);

        for (const BufferPtr& buffer : {gpu.vertices, gpu.indices})
        {
            if (!buffer->pending)
            {
                buffer->pending = true;
                m_Pending.push_back(buffer);
            }
        }
    }

    void ProceduralMeshRenderer::NotifySubmitted(uint64_t fence)
    {
        m_LastSubmittedFence = fence;
        for (const BufferPtr& buffer : m_Pending)
        {
            buffer->fence = std::max(buffer->fence, fence);
            buffer->pending = false;
        }
        m_Pending.clear();
        Collect();
    }

    void ProceduralMeshRenderer::ReleaseMesh(const ProceduralMesh& mesh)
    {
        const auto it = m_Meshes.find(&mesh);
        if (it == m_Meshes.end())
            return;
        Retire(std::move(it->second.vertices));
        Retire(std::move(it->second.indices));
        m_Meshes.erase(it);
        Collect();
    }

    void ProceduralMeshRenderer::Shutdown()
    {
        if (!m_Initialized && m_Meshes.empty() && m_Retired.empty() && m_Free.empty())
            return;
        // Shutdown is infrequent; all queued draws must complete before GPU resources are destroyed.
        if (g_CommandManager.GetGraphicsQueue().IsReady())
            g_CommandManager.GetGraphicsQueue().WaitForIdle();
        m_Pending.clear();
        m_Meshes.clear();
        m_Retired.clear();
        m_Free.clear();
        m_Initialized = false;
        m_PsoFinalized = false;
    }
}
