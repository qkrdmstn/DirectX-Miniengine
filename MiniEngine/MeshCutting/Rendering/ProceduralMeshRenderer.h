#pragma once

#include "../ProceduralMesh.h"
#include "../../Core/RootSignature.h"
#include "../../Core/PipelineState.h"
#include "../../Core/GpuBuffer.h"
#include "../../Core/VectorMath.h"
#include <memory>
#include <unordered_map>
#include <vector>

class GraphicsContext;
class ColorBuffer;
class DepthBuffer;

namespace MeshCutting
{
    // A mesh must outlive Draw calls and must be released before its address is reused.
    // Call NotifySubmitted with the fence returned by the context's Finish after every frame.
    class ProceduralMeshRenderer
    {
    public:
        ProceduralMeshRenderer();
        ~ProceduralMeshRenderer();
        ProceduralMeshRenderer(const ProceduralMeshRenderer&) = delete;
        ProceduralMeshRenderer& operator=(const ProceduralMeshRenderer&) = delete;

        enum class CullMode { TwoSided, Back };

        struct DrawOptions
        {
            CullMode cull = CullMode::TwoSided;     // Back: counter-clockwise faces only
            // Debug aid: draw each section in a fixed color (section 0 gray, 1 cyan, 2 yellow, ...)
            // so a cap can be told from the surface. Materials themselves are not supported yet.
            bool sectionTint = false;
        };

        void Initialize();
        void Draw(GraphicsContext& context, const ProceduralMesh& mesh,
            const Math::Matrix4& world, const Math::Matrix4& viewProj,
            ColorBuffer& color, DepthBuffer& depth,
            const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor,
            const DrawOptions& options = DrawOptions());
        void NotifySubmitted(uint64_t fence);
        void ReleaseMesh(const ProceduralMesh& mesh);
        void Shutdown();

    private:
        struct Buffer
        {
            ByteAddressBuffer resource;
            uint32_t capacityBytes = 0;
            uint64_t fence = 0;
            bool pending = false;
        };
        using BufferPtr = std::shared_ptr<Buffer>;
        struct MeshGpu
        {
            BufferPtr vertices;
            BufferPtr indices;
            uint64_t vertexVersion = 0;
            uint64_t topologyVersion = 0;
        };
        BufferPtr Acquire(uint32_t bytes);
        void Collect();
        void Retire(BufferPtr buffer);
        void Upload(GraphicsContext& context, Buffer& buffer, const void* data, uint32_t bytes);

        RootSignature m_RootSignature;
        GraphicsPSO m_Pso;              // two-sided
        GraphicsPSO m_PsoBackCull;      // back-face culling
        bool m_Initialized = false;
        bool m_PsoFinalized = false;
        DXGI_FORMAT m_ColorFormat = DXGI_FORMAT_UNKNOWN;
        DXGI_FORMAT m_DepthFormat = DXGI_FORMAT_UNKNOWN;
        std::unordered_map<const ProceduralMesh*, MeshGpu> m_Meshes;
        std::vector<BufferPtr> m_Retired;
        std::vector<BufferPtr> m_Free;
        std::vector<BufferPtr> m_Pending;
        uint64_t m_LastSubmittedFence = 0;
    };
}
