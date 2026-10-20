#include "../pch.h"
#include "CutDebugView.h"
#include "../Runtime/CutController.h"
#include "../Geometry/RenderBake.h"
#include "BufferManager.h"
#include "CommandContext.h"
#include "TextRenderer.h"
#include "Utility.h"

#include <algorithm>
#include <cmath>

using namespace Math;

namespace MeshCutting
{
    namespace
    {
        // Puts new vertex data into a mesh: positions only when the topology is unchanged (the usual
        // per-frame case), a full replacement otherwise. Empty data clears the mesh.
        void ReplaceOrUpdate(ProceduralMesh& mesh, ProceduralMeshData&& data)
        {
            std::string error;
            if (data.indices.empty())
            {
                if (!mesh.GetData().vertices.empty())
                    mesh.Clear();
            }
            else if (mesh.GetData().vertices.size() == data.vertices.size() && mesh.GetData().indices == data.indices)
                mesh.UpdateVertices(data.vertices, &error);
            else
                mesh.SetMesh(std::move(data), &error);
        }
    }

    namespace
    {
        // The plane ring shows the status of the dry-run: green cuts, gray touches nothing, red fails.
        DirectX::XMFLOAT4 PlaneColor(const CutController& cut)
        {
            if (!cut.HasPreview())
                return { 0.0f, 1.0f, 1.0f, 1.0f };
            switch (cut.PreviewStatus())
            {
            case SliceStatus::Ok: return { 0.1f, 1.0f, 0.1f, 1.0f };
            case SliceStatus::NoCut: return { 0.6f, 0.6f, 0.6f, 1.0f };
            default: return { 1.0f, 0.15f, 0.15f, 1.0f };
            }
        }
    }

    void CutDebugView::Sync(const CutController& cut)
    {
        std::string error;

        // A new patch (C) or none (X): the patch mesh and its boundary tubes start over.
        if (cut.PatchVersion() != m_PatchVersion)
        {
            m_PatchVersion = cut.PatchVersion();
            if (cut.ExtractedMesh().GetData().vertices.empty())
                m_PatchMesh.Clear();
            else
                m_PatchMesh.SetMesh(cut.ExtractedMesh().GetData(), &error);
            m_BoundaryDebug.Clear();
            if (cut.PatchStatus() == SliceStatus::Ok)
            {
                ProceduralMeshData tubes;
                BakeBoundaryDebug(cut.Patch(), cut.PatchRadius() * 0.008f, tubes);
                if (!tubes.indices.empty())
                    m_BoundaryDebug.SetMesh(std::move(tubes), &error);
            }
        }

        // The pieces were replaced (a new cut) or cleared, or Z changed how the sections are split:
        // bake them again. Only the index order depends on Z, so this never runs per frame.
        const bool piecesChanged = cut.PieceGeneration() != m_PieceGeneration
            || (cut.HasPieces() && m_BakedSectionSplit != m_SectionTint);
        if (piecesChanged)
        {
            m_PieceGeneration = cut.PieceGeneration();
            m_BakedSectionSplit = m_SectionTint;
            if (!cut.HasPieces())
            {
                m_PiecePositive.Clear();
                m_PieceNegative.Clear();
                m_PieceBoundaryPositive.Clear();
                m_PieceBoundaryNegative.Clear();
            }
            else
            {
                const float tubeRadius = cut.PatchRadius() * 0.008f;
                const auto bake = [&](const PatchPiece& piece, ProceduralMesh& mesh, ProceduralMesh& boundary)
                {
                    ProceduralMeshData data, tubes;
                    RenderBakeOptions bakeOptions;
                    bakeOptions.splitByOrigin = m_SectionTint;
                    BuildRenderMesh(piece, data, bakeOptions);
                    mesh.SetMesh(std::move(data), &error);
                    BakeBoundaryDebug(piece, tubeRadius, tubes);
                    if (tubes.indices.empty())
                        boundary.Clear();
                    else
                        boundary.SetMesh(std::move(tubes), &error);
                };
                bake(cut.SlicedPositive(), m_PiecePositive, m_PieceBoundaryPositive);
                bake(cut.SlicedNegative(), m_PieceNegative, m_PieceBoundaryNegative);
            }
        }

        // The dry-run was redone or cleared: show where the plane would cut. Only the loops are drawn.
        if (cut.PreviewGeneration() != m_PreviewGeneration)
        {
            m_PreviewGeneration = cut.PreviewGeneration();
            ProceduralMeshData loops;
            if (cut.HasPreview() && cut.PreviewStatus() == SliceStatus::Ok)
                BakeLoopDebug(cut.Preview().positive, cut.Preview().loops, cut.PatchRadius() * 0.012f,
                    { 1.0f, 0.2f, 1.0f, 1.0f }, loops);
            ReplaceOrUpdate(m_PreviewLoops, std::move(loops));
        }

        // The plane belongs to the world, not to the patch: it is rebuilt every frame while editing,
        // even when there is no patch to cut.
        if (cut.PlaneEditing() && cut.PlaneValid())
        {
            ProceduralMeshData planeMesh;
            BakeCutPlaneDebug(cut.PlaneFrame(), cut.ModelRadius() * 0.006f, PlaneColor(cut), planeMesh);
            ReplaceOrUpdate(m_PlaneDebug, std::move(planeMesh));
        }

        // Per-frame position refresh (only when the controller reached the end of its update).
        if (!cut.FramePoseValid())
            return;
        const float tubeRadius = cut.PatchRadius() * 0.008f;
        const auto refreshTubes = [&](const PatchPiece& piece, ProceduralMesh& mesh)
        {
            ProceduralMeshData tubes;
            BakeBoundaryDebug(piece, tubeRadius, tubes);
            ReplaceOrUpdate(mesh, std::move(tubes));
        };
        const auto refreshSurface = [&](const PatchPiece& piece, ProceduralMesh& mesh)
        {
            std::vector<ProceduralVertex> vertices = mesh.GetData().vertices;
            if (UpdateRenderPositions(piece, vertices))
                mesh.UpdateVertices(vertices, &error);
        };
        if (cut.HasPieces())
        {
            if (!piecesChanged)
            {
                refreshSurface(cut.SlicedPositive(), m_PiecePositive);
                refreshSurface(cut.SlicedNegative(), m_PieceNegative);
                if (m_BoundaryEnabled)
                {
                    refreshTubes(cut.SlicedPositive(), m_PieceBoundaryPositive);
                    refreshTubes(cut.SlicedNegative(), m_PieceBoundaryNegative);
                }
            }
        }
        else
        {
            refreshSurface(cut.Patch(), m_PatchMesh);
            if (m_BoundaryEnabled)
                refreshTubes(cut.Patch(), m_BoundaryDebug);
        }
    }

    void CutDebugView::Draw(GraphicsContext& context, ProceduralMeshRenderer& renderer, const CutController& cut,
        const Matrix4& viewProj, float modelRadius, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor)
    {
        Sync(cut);

        // Patch vertices are already in world space; shift the copy beside the original to compare them,
        // or (overlay) draw it exactly on the character to check that it follows the animation.
        const float patchOffset = m_OverlayEnabled ? 0.0f : modelRadius;
        const Matrix4 patchWorld(AffineTransform::MakeTranslation(Vector3(patchOffset, 0.0f, 0.0f)));
        ProceduralMeshRenderer::DrawOptions pieceOptions;
        pieceOptions.cull = m_BackCull ? ProceduralMeshRenderer::CullMode::Back : ProceduralMeshRenderer::CullMode::TwoSided;
        pieceOptions.sectionTint = m_SectionTint;
        const auto draw = [&](const ProceduralMesh& mesh, const Matrix4& world,
            const ProceduralMeshRenderer::DrawOptions& options = ProceduralMeshRenderer::DrawOptions())
        {
            renderer.Draw(context, mesh, world, viewProj, Graphics::g_SceneColorBuffer, Graphics::g_SceneDepthBuffer,
                viewport, scissor, options);
        };
        // Surfaces (patch and pieces) follow the U mode: solid, solid with lines on top, or lines only.
        // Dark lines read on the solid fill; over the dark scene they need to be light.
        const auto drawSurface = [&](const ProceduralMesh& mesh, const Matrix4& world,
            const ProceduralMeshRenderer::DrawOptions& options)
        {
            if (m_WireMode != WireMode::Wire)
                draw(mesh, world, options);
            if (m_WireMode != WireMode::Solid)
            {
                ProceduralMeshRenderer::DrawOptions wireOptions = options;
                wireOptions.fill = ProceduralMeshRenderer::FillMode::Wireframe;
                wireOptions.wireColor = m_WireMode == WireMode::Wire ? DirectX::XMFLOAT4(0.9f, 0.95f, 1.0f, 1.0f)
                    : DirectX::XMFLOAT4(0.05f, 0.05f, 0.05f, 1.0f);
                draw(mesh, world, wireOptions);
            }
        };

        if (cut.HasPieces())
        {
            // The pieces and the plane share the display offset; the pieces are pulled apart along
            // the plane normal so the cut can be seen.
            const float gap = m_OverlayEnabled ? 0.0f : cut.PatchRadius() * 0.05f;
            const DirectX::XMFLOAT3& n = cut.ShownNormal();
            const Vector3 shift = Vector3(n.x, n.y, n.z) * gap;
            const Matrix4 positiveWorld(AffineTransform::MakeTranslation(Vector3(patchOffset, 0.0f, 0.0f) + shift));
            const Matrix4 negativeWorld(AffineTransform::MakeTranslation(Vector3(patchOffset, 0.0f, 0.0f) - shift));
            drawSurface(m_PiecePositive, positiveWorld, pieceOptions);
            drawSurface(m_PieceNegative, negativeWorld, pieceOptions);
            if (m_BoundaryEnabled)
            {
                draw(m_PieceBoundaryPositive, positiveWorld);
                draw(m_PieceBoundaryNegative, negativeWorld);
            }
        }
        else
        {
            drawSurface(m_PatchMesh, patchWorld, ProceduralMeshRenderer::DrawOptions());
            if (m_BoundaryEnabled)
                draw(m_BoundaryDebug, patchWorld);
        }
        // The plane and the expected loops are in world space: draw them on the character, not beside it.
        if (cut.PlaneEditing())
        {
            const Matrix4 worldIdentity(kIdentity);
            draw(m_PlaneDebug, worldIdentity);
            draw(m_PreviewLoops, worldIdentity);
        }
    }

    void CutDebugView::DrawUI(TextContext& text, const CutController& cut) const
    {
        text.DrawFormattedString(L"C: copy joint patch  X: clear patch  %ls\n", cut.PatchStatusText().c_str());
        if (cut.Weld().corners > 0)
        {
            const WeldStats& weld = cut.Weld();
            text.DrawFormattedString(L"Weld: corners %u -> nodes %u (position-only %u, skin-split positions %u)\n",
                weld.corners, weld.nodes, weld.positionNodes, weld.skinSplitPositions);
            text.DrawFormattedString(L"G: boundary %ls  replacement %u (yellow)  unwelded seam %u (green)  non-manifold %u  %hs\n",
                m_BoundaryEnabled ? L"on" : L"off", weld.replacementEdges, weld.unweldedSeamEdges,
                weld.nonManifoldEdges, SliceStatusName(cut.PatchStatus()));
            text.DrawFormattedString(L"U: patch draw %ls\n", m_WireMode == WireMode::Solid ? L"solid"
                : (m_WireMode == WireMode::SolidAndWire ? L"solid + wire" : L"wire"));
        }
        if (cut.PatchStatus() != SliceStatus::Ok && !cut.PatchDetail().empty())
            text.DrawFormattedString(L"%ls\n", Utility::UTF8ToWideString(cut.PatchDetail().substr(0, 120)).c_str());
        {
            text.DrawFormattedString(L"T: cut mode %ls  R: reset plane  Enter: cut  Backspace: discard cut\n",
                cut.PlaneEditing() ? L"on" : L"off");
            text.DrawFormattedString(L"Move: Home/End up-down  Delete/PgDn left-right  [ ] along normal   Rotate: , . (U)  ; ' (V)  Ins/PgUp (normal)\n");
            text.DrawFormattedString(L"Size: - = all  Shift + - = : U only (Shift is also the fly camera's speed key)\n");
            if (cut.PlaneValid())
            {
                const CutPlaneFrame frame = cut.PlaneFrame();
                // Euler angles of the frame (x, y, z in degrees), for reading only.
                const float deg = 57.29578f;
                const float rx = atan2f(frame.axisV.z, frame.normal.z) * deg;
                const float ry = asinf(std::max(-1.0f, std::min(1.0f, -frame.axisU.z))) * deg;
                const float rz = atan2f(frame.axisU.y, frame.axisU.x) * deg;
                text.DrawFormattedString(L"Plane: center (%.2f %.2f %.2f)  rot ~(%.0f %.0f %.0f) deg  half %.2f x %.2f  normal (%.2f %.2f %.2f)\n",
                    frame.center.x, frame.center.y, frame.center.z, rx, ry, rz, frame.halfExtents.x, frame.halfExtents.y,
                    frame.normal.x, frame.normal.y, frame.normal.z);
            }
            text.DrawFormattedString(L"M: cap %ls (next cut)  L: cull %ls  Z: section tint %ls  N: overlay on character %ls\n",
                cut.CapEnabled() ? L"on" : L"off", m_BackCull ? L"back" : L"two-sided", m_SectionTint ? L"on" : L"off",
                m_OverlayEnabled ? L"on" : L"off");
            text.DrawFormattedString(L"Follow: %u nodes skinned, %.3f ms/frame  %hs\n", cut.SkinnedNodes(), cut.SkinMs(),
                cut.SkinError().empty() ? "" : cut.SkinError().substr(0, 100).c_str());
            if (cut.PlaneEditing())
            {
                if (cut.HasPreview())
                {
                    text.DrawFormattedString(L"Preview: %hs  loops %zu  %.3f ms\n", SliceStatusName(cut.PreviewStatus()),
                        cut.Preview().loops.size(), cut.PreviewMs());
                    if (cut.PreviewStatus() != SliceStatus::Ok && !cut.Preview().detail.empty())
                        text.DrawFormattedString(L"%ls\n", Utility::UTF8ToWideString(cut.Preview().detail.substr(0, 120)).c_str());
                }
                else
                    text.DrawFormattedString(L"Preview: -\n");
            }
            if (cut.CutAttempted())
            {
                text.DrawFormattedString(L"Cut: %hs  %ls  %.3f ms\n", SliceStatusName(cut.LastSliceStatus()),
                    cut.SliceInfo().c_str(), cut.CutMs());
                if (cut.LastSliceStatus() != SliceStatus::Ok && !cut.LastSliceDetail().empty())
                    text.DrawFormattedString(L"%ls\n", Utility::UTF8ToWideString(cut.LastSliceDetail().substr(0, 120)).c_str());
            }
        }
    }

    void CutDebugView::Release(ProceduralMeshRenderer& renderer)
    {
        renderer.ReleaseMesh(m_PatchMesh);
        renderer.ReleaseMesh(m_BoundaryDebug);
        renderer.ReleaseMesh(m_PiecePositive);
        renderer.ReleaseMesh(m_PieceNegative);
        renderer.ReleaseMesh(m_PieceBoundaryPositive);
        renderer.ReleaseMesh(m_PieceBoundaryNegative);
        renderer.ReleaseMesh(m_PlaneDebug);
        renderer.ReleaseMesh(m_PreviewLoops);
    }
}
