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
// Author:   James Stanard
//

#pragma once

#include "../Core/GpuBuffer.h"
#include "../Core/VectorMath.h"
#include "../Core/Camera.h"
#include "../Core/CommandContext.h"
#include "../Core/UploadBuffer.h"
#include "../Core/TextureManager.h"
#include <cstdint>
#include <memory>
#include <vector>

#include <d3d12.h>

class GraphicsPSO;
class RootSignature;
class DescriptorHeap;
class ShadowCamera;
class ShadowBuffer;
struct GlobalConstants;
struct Mesh;
struct Joint;

namespace Renderer
{
    extern BoolVar SeparateZPass;
    extern bool UseComputePose;

    using namespace Math;

    extern std::vector<GraphicsPSO> sm_PSOs;
    extern RootSignature m_RootSig;
    extern DescriptorHeap s_TextureHeap;
    extern DescriptorHeap s_SamplerHeap;
    extern DescriptorHandle m_CommonTextures;

    enum class RenderMode { Solid, Wireframe, Vertices };

    // 한 section의 draw 순서는 공유 Mesh.draw와 일치하며 baseVertex는 원본을 사용한다.
    struct MeshDrawRange
    {
        uint32_t StartIndex = 0;
        uint32_t IndexCount = 0;
    };

    // BufferLocation은 section의 절대 GPU 주소이며 mesh.ibOffset을 다시 더하지 않는다.
    struct MeshRenderOverride
    {
        D3D12_INDEX_BUFFER_VIEW IndexBuffer = {};
        std::vector<MeshDrawRange> Draws;
    };

    enum RootBindings
    {
        kMeshConstants,
        kMaterialConstants,
        kMaterialSRVs,
        kMaterialSamplers,
        kCommonSRVs,
        kCommonCBV,
        kSkinMatrices,
        kPosedVertices,

        kNumRootBindings
    };

    void Initialize(void);
    void Shutdown(void);

    uint8_t GetPSO(uint16_t psoFlags);
    void SetIBLTextures(TextureRef diffuseIBL, TextureRef specularIBL);
    void SetIBLBias(float LODBias);
    void UpdateGlobalDescriptors(void);
    void DrawSkybox( GraphicsContext& gfxContext, const Camera& camera, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor );

    class MeshSorter
    {
    public:
		enum BatchType { kDefault, kShadows };
        enum DrawPass { kZPass, kOpaque, kTransparent, kNumPasses };

        // 배치 종류와 메인 컬러 표시 모드를 보관하고 정렬 상태를 초기화한다.
		MeshSorter(BatchType type, RenderMode mode = RenderMode::Solid)
		{
			m_BatchType = type;
            m_RenderMode = mode;
			m_Camera = nullptr;
			m_Viewport = {};
			m_Scissor = {};
			m_NumRTVs = 0;
			m_DSV = nullptr;
			m_SortObjects.clear();
			m_SortKeys.clear();
			std::memset(m_PassCounts, 0, sizeof(m_PassCounts));
			m_CurrentPass = kZPass;
			m_CurrentDraw = 0;
		}

		void SetCamera( const BaseCamera& camera ) { m_Camera = &camera; }
		void SetViewport( const D3D12_VIEWPORT& viewport ) { m_Viewport = viewport; }
		void SetScissor( const D3D12_RECT& scissor ) { m_Scissor = scissor; }
		void AddRenderTarget( ColorBuffer& RTV )
		{ 
			ASSERT(m_NumRTVs < 8);
			m_RTV[m_NumRTVs++] = &RTV;
		}
		void SetDepthStencilTarget( DepthBuffer& DSV ) { m_DSV = &DSV; }

        const Frustum& GetWorldFrustum() const { return m_Camera->GetWorldSpaceFrustum(); }
        const Frustum& GetViewFrustum() const { return m_Camera->GetViewSpaceFrustum(); }
        const Matrix4& GetViewMatrix() const { return m_Camera->GetViewMatrix(); }

        // 원본 draw와 선택적 section override를 보관하고 렌더 패스별로 분류한다.
        // override는 모든 패스 동안 불변이며 정렬 목록이 공유 소유하여 수명을 보장한다.
        void AddMesh( const Mesh& mesh, float distance,
            D3D12_GPU_VIRTUAL_ADDRESS meshCBV,
            D3D12_GPU_VIRTUAL_ADDRESS materialCBV,
            D3D12_GPU_VIRTUAL_ADDRESS bufferPtr,
            const Joint* skeleton = nullptr, const GpuBuffer* posedBuffer = nullptr,
            uint32_t posedOffset = 0,
            std::shared_ptr<const MeshRenderOverride> renderOverride = nullptr);

        // override가 있으면 원본 IB view와 draw 범위를 대체한다.
        // GPU 버퍼 자체는 호출자가 제출된 명령의 완료까지 보존하고 INDEX_BUFFER 상태로 준비한다.

        void Sort();

        void RenderMeshes(DrawPass pass, GraphicsContext& context, GlobalConstants& globals);

    private:

        struct SortKey
        {
            union
            {
                uint64_t value;
                struct
                {
                    uint64_t objectIdx : 16;
                    uint64_t psoIdx : 12;
                    uint64_t key : 32;
                    uint64_t passID : 4;
                };
            };
        };

        struct SortObject
        {
            const Mesh* mesh;
            const Joint* skeleton;
            D3D12_GPU_VIRTUAL_ADDRESS meshCBV;
            D3D12_GPU_VIRTUAL_ADDRESS materialCBV;
            D3D12_GPU_VIRTUAL_ADDRESS bufferPtr;
            const GpuBuffer* posedBuffer;
            uint32_t posedOffset;
            std::shared_ptr<const MeshRenderOverride> renderOverride;
        };

        std::vector<SortObject> m_SortObjects;
        std::vector<uint64_t> m_SortKeys;
		BatchType m_BatchType;
        RenderMode m_RenderMode;
        uint32_t m_PassCounts[kNumPasses];
        DrawPass m_CurrentPass;
        uint32_t m_CurrentDraw;

		const BaseCamera* m_Camera;
		D3D12_VIEWPORT m_Viewport;
		D3D12_RECT m_Scissor;
		uint32_t m_NumRTVs;
		ColorBuffer* m_RTV[8];
		DepthBuffer* m_DSV;
	};

} // namespace Renderer
