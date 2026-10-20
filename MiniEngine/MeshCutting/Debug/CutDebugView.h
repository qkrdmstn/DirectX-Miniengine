#pragma once

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

    // Display side of the cutting demo: the debug meshes (patch, cut pieces, boundary tubes, plane),
    // the display toggles and the status lines. It only reads the controller.
    class CutDebugView
    {
    public:
        void ToggleBoundary() { m_BoundaryEnabled = !m_BoundaryEnabled; }    // G
        void ToggleBackCull() { m_BackCull = !m_BackCull; }                  // L
        void ToggleSectionTint() { m_SectionTint = !m_SectionTint; }         // Z
        void ToggleOverlay() { m_OverlayEnabled = !m_OverlayEnabled; }       // N

        // U: patch and cut pieces as Solid -> Solid + Wire -> Wire. Independent of the model's V wireframe.
        enum class WireMode { Solid, SolidAndWire, Wire };
        void CycleWireMode() { m_WireMode = WireMode((int(m_WireMode) + 1) % 3); }

        // Brings the meshes up to date with the controller (rebakes after a new patch or a new cut,
        // otherwise only refreshes positions) and draws them. modelRadius is the display offset of
        // the copy beside the character (0 without a model).
        void Draw(GraphicsContext& context, ProceduralMeshRenderer& renderer, const CutController& cut,
            const Math::Matrix4& viewProj, float modelRadius,
            const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor);

        // The status lines under the bone info, drawn with the caller's text context.
        // jointName turns a palette joint index into text (the view knows no Model).
        void DrawUI(TextContext& text, const CutController& cut, const std::function<std::wstring(uint32_t)>& jointName) const;

        // Gives the GPU buffers of every mesh back to the renderer (call before its Shutdown).
        void Release(ProceduralMeshRenderer& renderer);

    private:
        void Sync(const CutController& cut);

        ProceduralMesh m_PatchMesh;
        ProceduralMesh m_BoundaryDebug;
        ProceduralMesh m_PiecePositive, m_PieceNegative;
        ProceduralMesh m_PieceBoundaryPositive, m_PieceBoundaryNegative;
        ProceduralMesh m_PlaneDebug;
        ProceduralMesh m_PreviewLoops;          // where the current plane would cut (dry-run result)
        ProceduralMesh m_SegmentDebug;          // bone segments the plane crosses and where

        bool m_BoundaryEnabled = true;
        bool m_BackCull = false;                // back-face culling PSO instead of two-sided
        bool m_SectionTint = false;             // color surface and cap sections differently
        bool m_OverlayEnabled = false;          // draw exactly on the character (no display offset, no gap)
        WireMode m_WireMode = WireMode::Solid;

        uint64_t m_PatchVersion = 0;
        uint64_t m_PieceGeneration = 0;
        uint64_t m_PreviewGeneration = 0;
        bool m_BakedSectionSplit = false;       // the pieces were baked with splitByOrigin
    };
}
