#pragma once

#include "../Geometry/CutPlane.h"
#include "../ProceduralMesh.h"
#include "../Rendering/ProceduralMeshRenderer.h"
#include "VectorMath.h"
#include <cstdint>
#include <functional>
#include <string>

class GraphicsContext;
class TextContext;

namespace MeshCutting
{
    class CutController;

    // V: 렌더링 모드. 원본 모델과 잘린 조각에 같이 적용된다.
    enum class RenderMode
    {
        Solid,
        SolidAndWire,
        Wire,
        Debug       // Wire + 구역(section) 색 + 경계 튜브
    };

    // Display side of the cutting demo: the cut pieces, the plane, the crossed bone segments, the expected
    // loops and the gizmo, plus the status lines. It only reads the controller.
    class CutDebugView
    {
    public:
        void SetRenderMode(RenderMode mode) { m_RenderMode = mode; }
        void CycleRenderMode() { m_RenderMode = RenderMode((int(m_RenderMode) + 1) % 4); }
        RenderMode GetRenderMode() const { return m_RenderMode; }
        static const wchar_t* RenderModeName(RenderMode mode);

        // 기즈모를 그릴 때 필요한 카메라 정보. 프레임마다 Draw 전에 부른다. view의 크기는 창 클라이언트 영역이다.
        // 도구, 호버, 드래그 상태는 컨트롤러의 평면이 가지고 있다.
        void SetGizmoView(const GizmoView& view);

        // Brings the meshes up to date with the controller (rebakes after a new cut, otherwise only
        // refreshes positions) and draws them. modelRadius is the display offset of the cut pieces
        // beside the character (0 without a model). The plane, the bone segments, the loops and the
        // gizmo are in world space and drawn on the character.
        void Draw(GraphicsContext& context, ProceduralMeshRenderer& renderer, const CutController& cut,
            const Math::Matrix4& viewProj, float modelRadius,
            const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor);

        // 화면 텍스트: 도구, 렌더링 모드, 디버그 모드 여부, 절단 상태, 키 도움말.
        // jointName turns a palette joint index into text (the view knows no Model).
        void DrawUI(TextContext& text, const CutController& cut, const std::function<std::wstring(uint32_t)>& jointName) const;

        // Gives the GPU buffers of every mesh back to the renderer (call before its Shutdown).
        void Release(ProceduralMeshRenderer& renderer);

    private:
        void Sync(const CutController& cut);
        bool SectionTint() const { return m_RenderMode == RenderMode::Debug; }

        ProceduralMesh m_PiecePositive, m_PieceNegative;
        ProceduralMesh m_PieceBoundaryPositive, m_PieceBoundaryNegative;
        ProceduralMesh m_PlaneDebug;
        ProceduralMesh m_PreviewLoops;          // where the current plane would cut (dry-run result)
        ProceduralMesh m_SegmentDebug;          // bone segments the plane crosses and where
        ProceduralMesh m_GizmoMesh;

        RenderMode m_RenderMode = RenderMode::Solid;
        GizmoView m_GizmoView;
        bool m_HaveGizmoView = false;

        uint64_t m_PieceGeneration = 0;
        uint64_t m_PreviewGeneration = 0;
        bool m_BakedSectionSplit = false;       // the pieces were baked with splitByOrigin
    };
}
