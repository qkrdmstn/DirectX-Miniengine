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
    namespace
    {
        // Wireframe depth bias (reversed-Z: positive = toward the camera). The bias is in units of the
        // smallest depth step of the format, scaled by the polygon slope.
        constexpr INT kWireDepthBias = 200;
        constexpr float kWireSlopeScaledDepthBias = 1.5f;
    }

    ProceduralMeshRenderer::ProceduralMeshRenderer() :
        m_Pso(L"Procedural Mesh Color"),
        m_PsoBackCull(L"Procedural Mesh Color Back Cull"),
        m_PsoWire(L"Procedural Mesh Wire"),
        m_PsoWireBackCull(L"Procedural Mesh Wire Back Cull"),
        m_PsoNoDepth(L"Procedural Mesh No Depth")
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

        m_RootSignature.Reset(2, 0);
        m_RootSignature[0].InitAsConstantBuffer(0, D3D12_SHADER_VISIBILITY_VERTEX);
        // Tint: float4 color + uint mode + uint section index (see ProceduralMeshPS.hlsl).
        m_RootSignature[1].InitAsConstants(1, 6, D3D12_SHADER_VISIBILITY_PIXEL);
        m_RootSignature.Finalize(L"Procedural Mesh Root", D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);

        static const D3D12_INPUT_ELEMENT_DESC layout[] =
        {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MeshRenderVertex, position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(MeshRenderVertex, normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(MeshRenderVertex, uv), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(MeshRenderVertex, color), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        static_assert(sizeof(MeshRenderVertex) == 48, "Procedural vertex layout changed");

        m_Pso.SetRootSignature(m_RootSignature);
        m_Pso.SetRasterizerState(RasterizerTwoSided);
        m_Pso.SetBlendState(BlendDisable);
        m_Pso.SetDepthStencilState(DepthStateReadWrite);
        m_Pso.SetInputLayout(_countof(layout), layout);
        m_Pso.SetPrimitiveTopologyType(D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
        m_Pso.SetVertexShader(g_pProceduralMeshVS, sizeof(g_pProceduralMeshVS));
        m_Pso.SetPixelShader(g_pProceduralMeshPS, sizeof(g_pProceduralMeshPS));

        // Same state with back-face culling (RasterizerDefault: counter-clockwise is front).
        m_PsoBackCull = m_Pso;
        m_PsoBackCull.SetRasterizerState(RasterizerDefault);

        // Wireframe: tested against the depth buffer but never written, and pushed toward the camera.
        // MiniEngine is reversed-Z (GREATER_EQUAL, near = large depth), so a positive bias moves the
        // lines in front of a solid draw of the same mesh. Values are in the header comment of
        // CutDebugDesign.md; they were tuned on screen.
        D3D12_RASTERIZER_DESC wireTwoSided = RasterizerTwoSided;
        wireTwoSided.FillMode = D3D12_FILL_MODE_WIREFRAME;
        wireTwoSided.DepthBias = kWireDepthBias;
        wireTwoSided.SlopeScaledDepthBias = kWireSlopeScaledDepthBias;
        D3D12_RASTERIZER_DESC wireBackCull = RasterizerDefault;
        wireBackCull.FillMode = D3D12_FILL_MODE_WIREFRAME;
        wireBackCull.DepthBias = kWireDepthBias;
        wireBackCull.SlopeScaledDepthBias = kWireSlopeScaledDepthBias;
        m_PsoWire = m_Pso;
        m_PsoWire.SetRasterizerState(wireTwoSided);
        m_PsoWire.SetDepthStencilState(DepthStateReadOnly);
        m_PsoWireBackCull = m_Pso;
        m_PsoWireBackCull.SetRasterizerState(wireBackCull);
        m_PsoWireBackCull.SetDepthStencilState(DepthStateReadOnly);

        // Overlay (gizmo): no depth buffer at all, so it is always on top of the meshes. DepthStateDisabled needs
        // a PSO whose depth format is unknown (GraphicsPSO::Finalize asserts it); see Draw.
        m_PsoNoDepth = m_Pso;
        m_PsoNoDepth.SetDepthStencilState(DepthStateDisabled);
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
        const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor, const DrawOptions& options)
    {
        Initialize();
        if (!m_PsoFinalized || m_ColorFormat != color.GetFormat() || m_DepthFormat != depth.GetFormat())
        {
            m_ColorFormat = color.GetFormat();
            m_DepthFormat = depth.GetFormat();
            m_Pso.SetRenderTargetFormat(m_ColorFormat, m_DepthFormat);
            m_Pso.Finalize();
            m_PsoBackCull.SetRenderTargetFormat(m_ColorFormat, m_DepthFormat);
            m_PsoBackCull.Finalize();
            m_PsoWire.SetRenderTargetFormat(m_ColorFormat, m_DepthFormat);
            m_PsoWire.Finalize();
            m_PsoWireBackCull.SetRenderTargetFormat(m_ColorFormat, m_DepthFormat);
            m_PsoWireBackCull.Finalize();
            m_PsoNoDepth.SetRenderTargetFormat(m_ColorFormat, DXGI_FORMAT_UNKNOWN);    // no depth buffer bound
            m_PsoNoDepth.Finalize();
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

        const uint32_t vertexBytes = uint32_t(data.vertices.size() * sizeof(MeshRenderVertex));
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
        const bool wire = options.fill == FillMode::Wireframe;
        const bool backCull = options.cull == CullMode::Back;
        // An overlay (no depth test) is drawn without a depth buffer bound: the engine only accepts a PSO with
        // DepthStateDisabled when its depth format is unknown.
        const bool noDepth = !options.depthTest && !wire;
        if (noDepth)
            context.SetRenderTarget(color.GetRTV());
        else
        {
            context.TransitionResource(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, true);
            context.SetRenderTarget(color.GetRTV(), depth.GetDSV());
        }
        context.SetViewportAndScissor(viewport, scissor);
        context.SetRootSignature(m_RootSignature);
        if (noDepth)
            context.SetPipelineState(m_PsoNoDepth);
        else
            context.SetPipelineState(wire ? (backCull ? m_PsoWireBackCull : m_PsoWire) : (backCull ? m_PsoBackCull : m_Pso));
        context.SetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        struct alignas(16) Transforms { Math::Matrix4 world; Math::Matrix4 viewProj; };
        const Transforms transforms{ world, viewProj };
        context.SetDynamicConstantBufferView(0, sizeof(transforms), &transforms);
        context.SetVertexBuffer(0, gpu.vertices->resource.VertexBufferView(0, vertexBytes, sizeof(MeshRenderVertex)));
        context.SetIndexBuffer(gpu.indices->resource.IndexBufferView(0, indexBytes, true));
        // Pixel constants: color, a mode and the section index (ProceduralMeshPS.hlsl).
        //   0 vertex color   1 solid section color   2 one wire color   3 wire palette by section
        static const float kSectionTint[][4] = {
            { 0.85f, 0.85f, 0.85f, 1.0f }, { 0.1f, 0.8f, 0.9f, 1.0f }, { 0.95f, 0.8f, 0.1f, 1.0f },
            { 0.9f, 0.3f, 0.7f, 1.0f }, { 0.4f, 0.9f, 0.3f, 1.0f } };
        const uint32_t tintCount = uint32_t(_countof(kSectionTint));
        const auto setTint = [&context, &options, wire](uint32_t section)
        {
            struct { float color[4]; uint32_t mode; uint32_t section; } constants = {};
            constants.section = section;
            if (wire)
            {
                std::memcpy(constants.color, &options.wireColor, sizeof(constants.color));
                constants.mode = options.sectionTint ? 3 : 2;
            }
            else if (options.sectionTint)
            {
                std::memcpy(constants.color, kSectionTint[section % tintCount], sizeof(constants.color));
                constants.mode = 1;
            }
            context.SetConstantArray(1, 6, &constants);
        };
        if (data.sections.empty())
        {
            setTint(0);
            context.DrawIndexed(uint32_t(data.indices.size()));
        }
        else
        {
            uint32_t sectionIndex = 0;
            for (const MeshSection& section : data.sections)
            {
                if (section.indexCount != 0)
                {
                    setTint(sectionIndex);
                    context.DrawIndexed(section.indexCount, section.firstIndex);
                }
                ++sectionIndex;
            }
        }

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
