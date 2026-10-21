#include "../pch.h"
#include "CutDebugView.h"
#include "../Runtime/CutController.h"
#include "../Rendering/RenderBake.h"
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

        // The plane shows the status of the dry-run: green cuts, gray touches nothing, red fails.
        DirectX::XMFLOAT4 PlaneColor(const CutController& cut)
        {
            if (cut.NoSegment())
                return { 0.6f, 0.6f, 0.6f, 1.0f };
            if (!cut.HasPreview())
                return { 0.0f, 1.0f, 1.0f, 1.0f };
            switch (cut.PreviewStatus())
            {
            case SliceStatus::Ok: return { 0.1f, 1.0f, 0.1f, 1.0f };
            case SliceStatus::NoCut: return { 0.6f, 0.6f, 0.6f, 1.0f };
            default: return { 1.0f, 0.15f, 0.15f, 1.0f };
            }
        }

        // The cut plane as thin tubes: the rectangle border and a few cross lines in color, plus a white
        // arrow along the normal. out is cleared.
        void BakePlaneDebug(const CutPlane::Frame& frame, float tubeRadius, const DirectX::XMFLOAT4& color,
            ProceduralMeshData& out)
        {
            out = ProceduralMeshData();
            const DirectX::XMFLOAT4 arrowColor = { 1.0f, 1.0f, 1.0f, 1.0f };
            const float hx = frame.halfExtents.x, hy = frame.halfExtents.y;
            const auto at = [&](float a, float b, float c)
            {
                return DirectX::XMFLOAT3(frame.center.x + a * frame.axisU.x + b * frame.axisV.x + c * frame.normal.x,
                    frame.center.y + a * frame.axisU.y + b * frame.axisV.y + c * frame.normal.y,
                    frame.center.z + a * frame.axisU.z + b * frame.axisV.z + c * frame.normal.z);
            };

            // The rectangle border.
            AppendDebugSegment(out, at(-hx, -hy, 0.0f), at(hx, -hy, 0.0f), tubeRadius, color);
            AppendDebugSegment(out, at(hx, -hy, 0.0f), at(hx, hy, 0.0f), tubeRadius, color);
            AppendDebugSegment(out, at(hx, hy, 0.0f), at(-hx, hy, 0.0f), tubeRadius, color);
            AppendDebugSegment(out, at(-hx, hy, 0.0f), at(-hx, -hy, 0.0f), tubeRadius, color);

            // Cross lines inside: the two center lines and one at half the way to each side, thinner.
            const float fractions[3] = { -0.5f, 0.0f, 0.5f };
            for (float f : fractions)
            {
                AppendDebugSegment(out, at(-hx, f * hy, 0.0f), at(hx, f * hy, 0.0f), tubeRadius * 0.5f, color);
                AppendDebugSegment(out, at(f * hx, -hy, 0.0f), at(f * hx, hy, 0.0f), tubeRadius * 0.5f, color);
            }

            // The arrow points to the positive side (the side the plane normal points to).
            const float length = std::min(hx, hy) * 0.7f;
            const float head = length * 0.15f;
            AppendDebugSegment(out, at(0.0f, 0.0f, 0.0f), at(0.0f, 0.0f, length), tubeRadius, arrowColor);
            AppendDebugSegment(out, at(0.0f, 0.0f, length), at(head, 0.0f, length - 2.0f * head), tubeRadius, arrowColor);
            AppendDebugSegment(out, at(0.0f, 0.0f, length), at(-head, 0.0f, length - 2.0f * head), tubeRadius, arrowColor);
            AppendDebugSegment(out, at(0.0f, 0.0f, length), at(0.0f, head, length - 2.0f * head), tubeRadius, arrowColor);
            AppendDebugSegment(out, at(0.0f, 0.0f, length), at(0.0f, -head, length - 2.0f * head), tubeRadius, arrowColor);
            if (!out.indices.empty())
                out.sections.push_back({ 0, uint32_t(out.indices.size()), 0 });
        }

        const wchar_t* ToolName(GizmoTool tool)
        {
            switch (tool)
            {
            case GizmoTool::Move: return L"Move";
            case GizmoTool::Rotate: return L"Rotate";
            case GizmoTool::Scale: return L"Scale";
            case GizmoTool::Hidden: break;
            }
            return L"Hidden";
        }
    }

    const wchar_t* CutDebugView::RenderModeName(RenderMode mode)
    {
        switch (mode)
        {
        case RenderMode::Solid: return L"Solid";
        case RenderMode::SolidAndWire: return L"Solid + Wire";
        case RenderMode::Wire: return L"Wire";
        case RenderMode::Debug: break;
        }
        return L"Debug";
    }

    void CutDebugView::SetGizmoView(const GizmoView& view)
    {
        m_GizmoView = view;
        m_HaveGizmoView = true;
    }

    void CutDebugView::Sync(const CutController& cut)
    {
        std::string error;

        // The pieces were replaced (a new cut) or cleared, or the render mode changed how the sections are
        // split: bake them again. Only the index order depends on the mode, so this never runs per frame.
        const bool piecesChanged = cut.PieceGeneration() != m_PieceGeneration
            || (cut.HasPieces() && m_BakedSectionSplit != SectionTint());
        if (piecesChanged)
        {
            m_PieceGeneration = cut.PieceGeneration();
            m_BakedSectionSplit = SectionTint();
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
                    bakeOptions.splitByOrigin = SectionTint();
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
            BakePlaneDebug(cut.GetPlane().GetFrame(), cut.ModelRadius() * 0.006f, PlaneColor(cut), planeMesh);
            ReplaceOrUpdate(m_PlaneDebug, std::move(planeMesh));
        }
        else if (!m_PlaneDebug.GetData().vertices.empty())
            m_PlaneDebug.Clear();

        // The bone segments the plane crosses (yellow) and the crossing points (white crosses).
        if (cut.PlaneEditing() && cut.PlaneValid())
        {
            ProceduralMeshData segments;
            const float radius = cut.ModelRadius() * 0.01f;
            const SkeletonPose& skeleton = cut.Skeleton();
            for (const SegmentHit& hit : cut.Hits())
            {
                if (hit.parentJoint >= skeleton.jointPositions.size() || hit.childJoint >= skeleton.jointPositions.size())
                    continue;
                AppendDebugSegment(segments, skeleton.jointPositions[hit.parentJoint], skeleton.jointPositions[hit.childJoint],
                    radius, { 1.0f, 0.85f, 0.1f, 1.0f });
                const float arm = radius * 4.0f;
                const DirectX::XMFLOAT3& p = hit.point;
                const DirectX::XMFLOAT4 white = { 1.0f, 1.0f, 1.0f, 1.0f };
                AppendDebugSegment(segments, { p.x - arm, p.y, p.z }, { p.x + arm, p.y, p.z }, radius * 0.7f, white);
                AppendDebugSegment(segments, { p.x, p.y - arm, p.z }, { p.x, p.y + arm, p.z }, radius * 0.7f, white);
                AppendDebugSegment(segments, { p.x, p.y, p.z - arm }, { p.x, p.y, p.z + arm }, radius * 0.7f, white);
            }
            if (!segments.indices.empty())
                segments.sections.push_back({ 0, uint32_t(segments.indices.size()), 0 });
            ReplaceOrUpdate(m_SegmentDebug, std::move(segments));
        }
        else if (!m_SegmentDebug.GetData().vertices.empty())
            m_SegmentDebug.Clear();

        // The gizmo follows the plane; it is rebuilt every frame because its size depends on the camera.
        if (m_HaveGizmoView && cut.GetPlane().GetGizmoTool() != GizmoTool::Hidden && cut.PlaneEditing() && cut.PlaneValid())
        {
            ProceduralMeshData gizmo;
            cut.GetPlane().BakeGizmo(m_GizmoView, gizmo);
            ReplaceOrUpdate(m_GizmoMesh, std::move(gizmo));
        }
        else if (!m_GizmoMesh.GetData().vertices.empty())
            m_GizmoMesh.Clear();

        // Per-frame position refresh of the pieces (only when the controller reached the end of its update).
        if (!cut.FramePoseValid() || !cut.HasPieces() || piecesChanged)
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
        refreshSurface(cut.SlicedPositive(), m_PiecePositive);
        refreshSurface(cut.SlicedNegative(), m_PieceNegative);
        if (m_RenderMode == RenderMode::Debug)
        {
            refreshTubes(cut.SlicedPositive(), m_PieceBoundaryPositive);
            refreshTubes(cut.SlicedNegative(), m_PieceBoundaryNegative);
        }
    }

    void CutDebugView::Draw(GraphicsContext& context, ProceduralMeshRenderer& renderer, const CutController& cut,
        const Matrix4& viewProj, float modelRadius, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor)
    {
        Sync(cut);

        // The cut pieces are drawn beside the character (shifted by the model radius along X) and pulled
        // apart along the plane normal, so the cut can be seen.
        ProceduralMeshRenderer::DrawOptions pieceOptions;
        pieceOptions.cull = ProceduralMeshRenderer::CullMode::TwoSided;
        pieceOptions.sectionTint = SectionTint();
        const auto draw = [&](const ProceduralMesh& mesh, const Matrix4& world,
            const ProceduralMeshRenderer::DrawOptions& options = ProceduralMeshRenderer::DrawOptions())
        {
            renderer.Draw(context, mesh, world, viewProj, Graphics::g_SceneColorBuffer, Graphics::g_SceneDepthBuffer,
                viewport, scissor, options);
        };
        // Pieces follow the render mode: solid, solid with dark lines on top, or light lines only (Debug
        // adds the section colors and the boundary tubes). Dark lines read on the solid fill; over the
        // dark scene they need to be light.
        const auto drawSurface = [&](const ProceduralMesh& mesh, const Matrix4& world,
            const ProceduralMeshRenderer::DrawOptions& options)
        {
            const bool wireOnly = m_RenderMode == RenderMode::Wire || m_RenderMode == RenderMode::Debug;
            if (!wireOnly)
                draw(mesh, world, options);
            if (m_RenderMode != RenderMode::Solid)
            {
                ProceduralMeshRenderer::DrawOptions wireOptions = options;
                wireOptions.fill = ProceduralMeshRenderer::FillMode::Wireframe;
                wireOptions.wireColor = wireOnly ? DirectX::XMFLOAT4(0.9f, 0.95f, 1.0f, 1.0f)
                    : DirectX::XMFLOAT4(0.05f, 0.05f, 0.05f, 1.0f);
                draw(mesh, world, wireOptions);
            }
        };

        if (cut.HasPieces())
        {
            const float gap = cut.PatchRadius() * 0.05f;
            const DirectX::XMFLOAT3& n = cut.ShownNormal();
            const Vector3 shift = Vector3(n.x, n.y, n.z) * gap;
            const Vector3 offset(modelRadius, 0.0f, 0.0f);
            const Matrix4 positiveWorld(AffineTransform::MakeTranslation(offset + shift));
            const Matrix4 negativeWorld(AffineTransform::MakeTranslation(offset - shift));
            drawSurface(m_PiecePositive, positiveWorld, pieceOptions);
            drawSurface(m_PieceNegative, negativeWorld, pieceOptions);
            if (m_RenderMode == RenderMode::Debug)
            {
                draw(m_PieceBoundaryPositive, positiveWorld);
                draw(m_PieceBoundaryNegative, negativeWorld);
            }
        }

        // The plane, the expected loops, the crossed bone segments and the gizmo are in world space:
        // draw them on the character. The gizmo ignores the depth buffer so it is never hidden.
        if (cut.PlaneEditing())
        {
            const Matrix4 worldIdentity(kIdentity);
            draw(m_PlaneDebug, worldIdentity);
            draw(m_PreviewLoops, worldIdentity);
            draw(m_SegmentDebug, worldIdentity);
            ProceduralMeshRenderer::DrawOptions overlay;
            overlay.depthTest = false;
            draw(m_GizmoMesh, worldIdentity, overlay);
        }
    }

    void CutDebugView::DrawUI(TextContext& text, const CutController& cut, const std::function<std::wstring(uint32_t)>& jointName) const
    {
        text.DrawFormattedString(L"Debug mode %ls (T)   Tool: %ls (W/E/R/Q)   Render: %ls (V)\n",
            cut.PlaneEditing() ? L"ON" : L"off", ToolName(cut.GetPlane().GetGizmoTool()), RenderModeName(m_RenderMode));

        if (cut.PlaneEditing())
        {
            // What the plane crosses, the joints that will be cut (the weight view shows the first), and the
            // time the candidate patch took.
            std::wstring joints;
            for (uint32_t joint : cut.CutJoints())
                joints += L" " + std::to_wstring(joint) + L"(" + jointName(joint) + L")";
            text.DrawFormattedString(L"Cut joints:%ls\n",
                joints.empty() ? L" none (no bone segment crosses the plane)" : joints.c_str());
            text.DrawFormattedString(L"Candidate patch %.3f ms (%u extractions)   follow %.3f ms\n", cut.ExtractMs(),
                cut.ExtractCount(), cut.SkinMs());
            if (!cut.CandidateError().empty())
                text.DrawFormattedString(L"%ls\n", cut.CandidateError().c_str());
            else if (cut.PatchStatus() != SliceStatus::Ok && !cut.CutJoints().empty())
                text.DrawFormattedString(L"%ls %ls\n", cut.PatchStatusText().c_str(),
                    Utility::UTF8ToWideString(cut.PatchDetail().substr(0, 100)).c_str());

            if (cut.HasPreview())
            {
                text.DrawFormattedString(L"Preview: %hs   loops %zu   %.3f ms\n", SliceStatusName(cut.PreviewStatus()),
                    cut.Preview().loops.size(), cut.PreviewMs());
                if (cut.PreviewStatus() != SliceStatus::Ok && !cut.Preview().detail.empty())
                    text.DrawFormattedString(L"  %ls\n", Utility::UTF8ToWideString(cut.Preview().detail.substr(0, 120)).c_str());
            }
        }
        if (cut.CutAttempted())
        {
            text.DrawFormattedString(L"Cut: %hs   %ls   %.3f ms\n", SliceStatusName(cut.LastSliceStatus()),
                cut.SliceInfo().c_str(), cut.CutMs());
            if (cut.LastSliceStatus() != SliceStatus::Ok && !cut.LastSliceDetail().empty())
                text.DrawFormattedString(L"  %ls\n", Utility::UTF8ToWideString(cut.LastSliceDetail().substr(0, 120)).c_str());
        }

        text.DrawFormattedString(L"Space: animation   T: debug mode   V: render mode   Enter: cut   Ctrl+Z: undo cut   W/E/R/Q: gizmo tool\n");
        text.DrawFormattedString(L"RMB+WASD/QE: fly (Shift: fast, wheel: speed)   MMB: pan   Alt+LMB: orbit   Wheel: zoom   F: frame\n");
    }

    void CutDebugView::Release(ProceduralMeshRenderer& renderer)
    {
        renderer.ReleaseMesh(m_PiecePositive);
        renderer.ReleaseMesh(m_PieceNegative);
        renderer.ReleaseMesh(m_PieceBoundaryPositive);
        renderer.ReleaseMesh(m_PieceBoundaryNegative);
        renderer.ReleaseMesh(m_PlaneDebug);
        renderer.ReleaseMesh(m_PreviewLoops);
        renderer.ReleaseMesh(m_SegmentDebug);
        renderer.ReleaseMesh(m_GizmoMesh);
    }
}
