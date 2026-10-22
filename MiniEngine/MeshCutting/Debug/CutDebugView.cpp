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

        // 평면 색은 교차 후보 유무만 표시한다. 절단 성공/실패는 Enter 뒤 상태 줄에서 확인한다.
        DirectX::XMFLOAT4 PlaneColor(const CutController& cut)
        {
            for (const auto& entry : cut.GetJobs())
            {
                if (entry.second.candidate)
                    return { 0.0f, 1.0f, 1.0f, 1.0f };
            }
            return { 0.6f, 0.6f, 0.6f, 1.0f };
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


    void CutDebugView::Sync(ProceduralMeshRenderer& renderer, const CutController& cut)
    {
        std::string error;
        const std::map<CutJobKey, CutJob>& jobs = cut.GetJobs();

        // 사라진 작업의 메시는 렌더러에 돌려준 뒤 지운다 (렌더러는 메시 주소로 GPU 버퍼를 기억한다).
        for (auto it = m_JobMeshes.begin(); it != m_JobMeshes.end();)
        {
            if (jobs.find(it->first) != jobs.end())
            {
                ++it;
                continue;
            }
            if (it->second.positive) renderer.ReleaseMesh(*it->second.positive);
            if (it->second.negative) renderer.ReleaseMesh(*it->second.negative);
            renderer.ReleaseMesh(it->second.boundaryPositive);
            renderer.ReleaseMesh(it->second.boundaryNegative);
            it = m_JobMeshes.erase(it);
        }

        for (const auto& entry : jobs)
        {
            const CutJob& job = entry.second;
            JobMeshes& meshes = m_JobMeshes[entry.first];
            const CutTarget* jobTarget = cut.FindTarget(job.key.target);
            const float targetRadius = jobTarget != nullptr ? jobTarget->radius : cut.GetModelRadius();

            // 새 조각은 같은 객체를 참조하고, 표시 옵션과 경계 튜브만 맞춘다.
            const bool piecesChanged = job.confirmedGeneration != meshes.pieceGeneration
                || (job.haveConfirmed && meshes.bakedSectionSplit != SectionSplit());
            if (piecesChanged)
            {
                meshes.pieceGeneration = job.confirmedGeneration;
                meshes.bakedSectionSplit = SectionSplit();
                const auto retain = [&](std::shared_ptr<ProceduralMesh>& mesh, const std::shared_ptr<ProceduralMesh>& next)
                {
                    if (mesh != next && mesh) renderer.ReleaseMesh(*mesh);
                    mesh = next;
                };
                retain(meshes.positive, job.positive);
                retain(meshes.negative, job.negative);
                if (!job.haveConfirmed)
                {
                    meshes.boundaryPositive.Clear();
                    meshes.boundaryNegative.Clear();
                }
                else
                {
                    const float tubeRadius = targetRadius * 0.008f;
                    meshes.positive->SetSectionSplit(SectionSplit(), &error);
                    meshes.negative->SetSectionSplit(SectionSplit(), &error);
                    const auto bake = [&](const MeshGeometry& piece, ProceduralMesh& boundary)
                    {
                        ProceduralMeshData tubes;
                        BakeBoundaryDebug(piece, tubeRadius, tubes);
                        if (tubes.indices.empty())
                            boundary.Clear();
                        else
                            boundary.SetMesh(std::move(tubes), &error);
                    };
                    bake(meshes.positive->GetGeometry(), meshes.boundaryPositive);
                    bake(meshes.negative->GetGeometry(), meshes.boundaryNegative);
                }
            }

            // 표면 위치는 메시가 갱신했다. 여기서는 디버그 경계 튜브만 갱신한다.
            if (!job.poseValid || !job.haveConfirmed || piecesChanged)
                continue;
            const float tubeRadius = targetRadius * 0.008f;
            const auto refreshTubes = [&](const MeshGeometry& piece, ProceduralMesh& mesh)
            {
                ProceduralMeshData tubes;
                BakeBoundaryDebug(piece, tubeRadius, tubes);
                ReplaceOrUpdate(mesh, std::move(tubes));
            };
            if (m_RenderMode == RenderMode::Debug)
            {
                refreshTubes(meshes.positive->GetGeometry(), meshes.boundaryPositive);
                refreshTubes(meshes.negative->GetGeometry(), meshes.boundaryNegative);
            }
        }

        // 평면은 패치가 아니라 월드에 속한다. 편집 중에는 자를 패치가 없어도 매 프레임 만든다.
        if (cut.PlaneEditing() && cut.PlaneValid())
        {
            ProceduralMeshData planeMesh;
            BakePlaneDebug(cut.GetPlane().GetFrame(), cut.GetModelRadius() * 0.006f, PlaneColor(cut), planeMesh);
            ReplaceOrUpdate(m_PlaneDebug, std::move(planeMesh));
        }
        else if (!m_PlaneDebug.GetData().vertices.empty())
            m_PlaneDebug.Clear();

        // 평면이 지나는 뼈 선분(노랑)과 교차점(흰 십자). 작업과 무관한 표시용이고, 모든 대상·팔레트를 한 메시에 모은다.
        if (cut.PlaneEditing() && cut.PlaneValid())
        {
            ProceduralMeshData segments;
            const float radius = cut.GetModelRadius() * 0.01f;
            for (const CutTarget& target : cut.GetTargets())
            {
                for (size_t slot = 0; slot < target.segmentHits.size(); ++slot)
                {
                    const SkeletonPose& skeleton = target.skeletons[slot];
                    for (const SegmentHit& hit : target.segmentHits[slot])
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
                }
            }
            if (!segments.indices.empty())
                segments.sections.push_back({ 0, uint32_t(segments.indices.size()), 0 });
            ReplaceOrUpdate(m_SegmentDebug, std::move(segments));
        }
        else if (!m_SegmentDebug.GetData().vertices.empty())
            m_SegmentDebug.Clear();

        // 기즈모는 평면을 따라가고, 크기가 카메라에 따라 달라서 매 프레임 만든다.
        if (m_HaveGizmoView && cut.GetPlane().GetGizmoTool() != GizmoTool::Hidden && cut.PlaneEditing() && cut.PlaneValid())
        {
            ProceduralMeshData gizmo;
            cut.GetPlane().BakeGizmo(m_GizmoView, gizmo);
            ReplaceOrUpdate(m_GizmoMesh, std::move(gizmo));
        }
        else if (!m_GizmoMesh.GetData().vertices.empty())
            m_GizmoMesh.Clear();
    }

    void CutDebugView::Draw(GraphicsContext& context, ProceduralMeshRenderer& renderer, const CutController& cut,
        const Matrix4& viewProj, const D3D12_VIEWPORT& viewport, const D3D12_RECT& scissor)
    {
        Sync(renderer, cut);

        ProceduralMeshRenderer::DrawOptions pieceOptions;
        pieceOptions.cull = ProceduralMeshRenderer::CullMode::TwoSided;
        const auto draw = [&](const ProceduralMesh& mesh, const Matrix4& world,
            const ProceduralMeshRenderer::DrawOptions& options = ProceduralMeshRenderer::DrawOptions())
        {
            renderer.Draw(context, mesh, world, viewProj, Graphics::g_SceneColorBuffer, Graphics::g_SceneDepthBuffer,
                viewport, scissor, options);
        };
        // 조각은 렌더링 모드를 따른다: Solid, Solid + 어두운 선, 밝은 선만(Wire). Debug는 선 색이 작업 색이다.
        const auto drawSurface = [&](const ProceduralMesh& mesh, const Matrix4& world,
            const ProceduralMeshRenderer::DrawOptions& options, const DirectX::XMFLOAT4& jobColor)
        {
            const bool wireOnly = m_RenderMode == RenderMode::Wire || m_RenderMode == RenderMode::Debug;
            if (!wireOnly)
                draw(mesh, world, options);
            if (m_RenderMode != RenderMode::Solid)
            {
                ProceduralMeshRenderer::DrawOptions wireOptions = options;
                wireOptions.fill = ProceduralMeshRenderer::FillMode::Wireframe;
                wireOptions.wireColor = m_RenderMode == RenderMode::Debug ? jobColor
                    : (wireOnly ? DirectX::XMFLOAT4(0.9f, 0.95f, 1.0f, 1.0f) : DirectX::XMFLOAT4(0.05f, 0.05f, 0.05f, 1.0f));
                draw(mesh, world, wireOptions);
            }
        };

        // 조각은 작업마다 따로 X 방향 한 줄에 나란히 놓는다 (겹치지 않게). 첫 작업은 대상 옆(대상 반지름만큼 X로 이동)에서
        // 시작하고, 다음 작업은 앞 작업의 오른쪽 끝에 이어 붙인다. 각 작업은 평면 법선으로 두 조각을 벌려서 절단을 볼 수 있게 한다.
        bool firstPlaced = false;
        float rowEnd = 0.0f;        // 지금까지 놓은 조각들의 오른쪽 끝 x
        for (const auto& entry : cut.GetJobs())
        {
            const CutJob& job = entry.second;
            const auto found = m_JobMeshes.find(entry.first);
            if (!job.haveConfirmed || found == m_JobMeshes.end())
                continue;
            const JobMeshes& meshes = found->second;
            const CutTarget* target = cut.FindTarget(job.key.target);
            const float radius = target != nullptr ? target->radius : cut.GetModelRadius();
            const DirectX::XMFLOAT4 color = GetJobColor(job.colorIndex);
            if (job.breakNode != kInvalidIndex)
            {
                // 끊긴 작업의 조각은 스키닝된 자리에 그대로 그린다 (남는 조각은 몸에, 떨어지는 조각은 떨어진 팔에 붙는다)
                const Matrix4 worldIdentity(kIdentity);
                drawSurface(*meshes.positive, worldIdentity, pieceOptions, color);
                drawSurface(*meshes.negative, worldIdentity, pieceOptions, color);
                if (m_RenderMode == RenderMode::Debug)
                {
                    draw(meshes.boundaryPositive, worldIdentity);
                    draw(meshes.boundaryNegative, worldIdentity);
                }
                continue;
            }
            const float gap = radius * 0.05f;
            const DirectX::XMFLOAT3& n = job.confirmedNormal;
            const Vector3 shift = Vector3(n.x, n.y, n.z) * gap;
            // 두 조각 경계의 중심으로 가로 위치를 정한다: 중심의 x를 슬롯 중앙에 맞추고, y와 z는 원래 높이를 유지한다.
            const ProceduralMeshBounds& positiveBounds = meshes.positive->GetBounds();
            const ProceduralMeshBounds& negativeBounds = meshes.negative->GetBounds();
            const float centerX = positiveBounds.valid && negativeBounds.valid ? (positiveBounds.center.x + negativeBounds.center.x) * 0.5f
                : (positiveBounds.valid ? positiveBounds.center.x : negativeBounds.center.x);
            const float halfWidth = radius * 1.15f;
            const float slotCenter = firstPlaced ? rowEnd + halfWidth : centerX + (target != nullptr ? target->radius : 0.0f);
            rowEnd = slotCenter + halfWidth;
            firstPlaced = true;
            const Vector3 offset(slotCenter - centerX, 0.0f, 0.0f);
            const Matrix4 positiveWorld(AffineTransform::MakeTranslation(offset + shift));
            const Matrix4 negativeWorld(AffineTransform::MakeTranslation(offset - shift));
            drawSurface(*meshes.positive, positiveWorld, pieceOptions, color);
            drawSurface(*meshes.negative, negativeWorld, pieceOptions, color);
            if (m_RenderMode == RenderMode::Debug)
            {
                draw(meshes.boundaryPositive, positiveWorld);
                draw(meshes.boundaryNegative, negativeWorld);
            }
        }

        // 평면, 교차 선분, 기즈모는 월드 공간이라 캐릭터 위에 그린다. 기즈모는 깊이를 무시해서 가려지지 않는다.
        if (cut.PlaneEditing())
        {
            const Matrix4 worldIdentity(kIdentity);
            draw(m_PlaneDebug, worldIdentity);
            draw(m_SegmentDebug, worldIdentity);
            ProceduralMeshRenderer::DrawOptions overlay;
            overlay.depthTest = false;
            draw(m_GizmoMesh, worldIdentity, overlay);
        }
    }

    void CutDebugView::DrawUI(TextContext& text, const CutController& cut, const JointNameFunction& jointName) const
    {
        text.DrawFormattedString(L"Debug mode %ls (T)   Tool: %ls (W/E/R/Q)   Render: %ls (V)\n",
            cut.PlaneEditing() ? L"ON" : L"off", ToolName(cut.GetPlane().GetGizmoTool()), RenderModeName(m_RenderMode));

        const CutFrameStats stats = cut.GetFrameStats();
        if (cut.PlaneEditing())
        {
            text.DrawFormattedString(L"Jobs %u (candidates %u, confirmed %u)   pick %.3f ms   extract %.3f ms (%u extractions)   follow %.3f ms   cut %.3f ms\n",
                stats.jobs, stats.candidates, stats.confirmed, stats.pickMs, stats.extractMs, stats.extractCount, stats.skinMs, stats.cutMs);
            // 계측: 이번 프레임 개수와 단계별 시간 (이번 프레임 / 지수 평균, ms). 평균에는 건너뛴 프레임의 0이 들어간다.
            text.DrawFormattedString(L"Count: targets %u pal %u  skinned verts cache %u job %u  tris checked %u mixed %u crossed %u\n",
                stats.targets, stats.palettes, stats.cacheVertices, stats.jobVertices, stats.checkedTriangles, stats.mixedTriangles, stats.crossedTriangles);
            text.DrawFormattedString(L"Static: meshes %u culled %u (bounds)  tris checked %u\n", stats.staticMeshes, stats.staticCulled, stats.staticTriangles);
            text.DrawFormattedString(L"Jobs +%u =%u -%u  extractions %u   pick refresh: pose %u plane %u both %u skipped %u\n",
                stats.jobsAdded, stats.jobsKept, stats.jobsLost, stats.extractions, stats.refreshPose, stats.refreshPlane, stats.refreshBoth, stats.refreshSkipped);
            text.DrawFormattedString(L"ms now/avg: skeleton %.2f/%.2f  pickMat %.2f/%.2f  pickSkin %.2f/%.2f  pickTris %.2f/%.2f  segments %.2f/%.2f\n",
                stats.skeleton.frame, stats.skeleton.average, stats.poseMatrices.frame, stats.poseMatrices.average, stats.cacheSkin.frame, stats.cacheSkin.average,
                stats.pick.frame, stats.pick.average, stats.segments.frame, stats.segments.average);
            text.DrawFormattedString(L"            jobUpdate %.2f/%.2f  jobMat %.2f/%.2f  jobSkin %.2f/%.2f\n",
                stats.jobUpdate.frame, stats.jobUpdate.average, stats.jobMatrices.frame, stats.jobMatrices.average,
                stats.jobSkin.frame, stats.jobSkin.average);
            text.DrawFormattedString(L"Enter ms now/avg: extract %.2f/%.2f  skin %.2f/%.2f  cut %.2f/%.2f  confirm total %.2f/%.2f\n",
                stats.extract.frame, stats.extract.average, stats.jobSkin.frame, stats.jobSkin.average, stats.cut.frame, stats.cut.average,
                stats.confirm.frame, stats.confirm.average);
            for (const CutTarget& target : cut.GetTargets())
            {
                if (!target.error.empty())
                    text.DrawFormattedString(L"[%ls] %ls\n", target.name.c_str(), target.error.c_str());
                for (size_t slot = 0; slot < target.sources.size(); ++slot)
                    if (!target.sources[slot].error.empty())
                        text.DrawFormattedString(L"[%ls] pal %u source: %ls\n", target.name.c_str(), target.palettes[slot], target.sources[slot].error.c_str());
                for (const CutStaticMesh& mesh : target.staticMeshes)
                    if (!mesh.error.empty())
                        text.DrawFormattedString(L"[%ls] mesh %u source: %ls\n", target.name.c_str(), mesh.meshIndex, mesh.error.c_str());
            }
            if (stats.candidates == 0)
                text.DrawFormattedString(L"No triangle crosses the plane\n");
        }

        for (const auto& entry : cut.GetJobs())
        {
            const CutJob& job = entry.second;
            if (!cut.PlaneEditing() && !job.haveConfirmed && !job.cutAttempted)
                continue;
            const CutTarget* target = cut.FindTarget(job.key.target);
            const std::wstring targetName = target != nullptr ? target->name : L"?";
            std::wstring line = L"#" + std::to_wstring(job.colorIndex + 1) + L" [" + targetName + L"]";
            if (job.key.kind == CutJobKind::StaticMesh)
                line += L" mesh " + std::to_wstring(job.key.a);
            else
                line += L" pal " + std::to_wstring(job.key.a) + L"  joint " + std::to_wstring(job.key.b)
                    + L"(" + jointName(job.key.target, job.key.a, job.key.b) + L")";
            if (job.candidate)
            {
                line += L"  crossed " + std::to_wstring(job.pickTriangles) + L" tris";
            }
            if (job.haveConfirmed)
                line += L"  patch " + std::to_wstring(job.patchTriangles) + L" tris";
            if (job.cutAttempted)
                line += L"  cut " + Utility::UTF8ToWideString(SliceStatusName(job.lastCutStatus));
            else if (job.haveConfirmed)
                line += L"  cut Ok";
            if (!job.candidate)
                line += L"  (not under the plane)";
            text.DrawFormattedString(L"%ls\n", line.c_str());

            const auto detail = [&](const std::string& message)
            {
                if (!message.empty())
                    text.DrawFormattedString(L"    %ls\n", Utility::UTF8ToWideString(message.substr(0, 110)).c_str());
            };
            detail(job.skinError);
            if (job.cutAttempted && job.lastCutStatus != SliceStatus::Ok)
                detail(job.lastCutError);
            else if (job.haveConfirmed && !job.cutInfo.empty())
                text.DrawFormattedString(L"    %ls   %.3f ms\n", job.cutInfo.c_str(), job.cutMs);
        }

        text.DrawFormattedString(L"Space: animation   T: debug mode   V: render mode   Enter: cut   Ctrl+Z: undo cut   W/E/R/Q: gizmo tool\n");
        text.DrawFormattedString(L"RMB+WASD/QE: fly (Shift: fast, wheel: speed)   MMB: pan   Alt+LMB: orbit   Wheel: zoom   F: frame\n");
    }

    void CutDebugView::Release(ProceduralMeshRenderer& renderer)
    {
        for (auto& entry : m_JobMeshes)
        {
            if (entry.second.positive) renderer.ReleaseMesh(*entry.second.positive);
            if (entry.second.negative) renderer.ReleaseMesh(*entry.second.negative);
            renderer.ReleaseMesh(entry.second.boundaryPositive);
            renderer.ReleaseMesh(entry.second.boundaryNegative);
        }
        m_JobMeshes.clear();
        renderer.ReleaseMesh(m_PlaneDebug);
        renderer.ReleaseMesh(m_SegmentDebug);
        renderer.ReleaseMesh(m_GizmoMesh);
    }
}
