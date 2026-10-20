#include "../pch.h"
#include "CutController.h"
#include "../../Model/Model.h"
#include "EngineProfiling.h"
#include "SystemTime.h"
#include "Utility.h"

using namespace Math;

namespace MeshCutting
{
    bool CutController::SetPatch(const Model& model, const ModelInstance& instance, uint32_t paletteId, uint32_t jointIndex)
    {
        // Copies with the pose of the last ModelInstance::Update, i.e. what is on screen now.
        ProceduralMeshData patch;
        BonePatchSource source;
        std::string error;
        if (ExtractBonePatch(model, instance, paletteId, jointIndex, 0.0f, patch, source, &error)
            && m_ExtractedMesh.SetMesh(std::move(patch), &error))
        {
            m_Source = std::move(source);
            m_PatchStatusText = L"Patch: " + std::to_wstring(m_ExtractedMesh.GetData().indices.size() / 3) + L" tris, "
                + std::to_wstring(m_ExtractedMesh.GetData().vertices.size()) + L" verts, mixed outside "
                + std::to_wstring(m_Source.mixedOutsideTriangles);
            RebuildPatchPiece();
            ClearSlice();
            ClearPreview();
            m_PreviousPoseJointWorld.clear();
            m_CurrentPoseJointWorld = m_Source.poseJointWorld;
            m_PreviewDirty = m_PlaneEditing;    // no automatic cut: the preview follows, Enter cuts
            ++m_PatchVersion;
            return true;
        }
        m_PatchStatusText = L"Patch failed: " + Utility::UTF8ToWideString(error);
        return false;
    }

    void CutController::ClearPatch()
    {
        m_ExtractedMesh.Clear();
        m_Source = {};
        m_PatchStatusText.clear();
        RebuildPatchPiece();
        ClearSlice();
        ClearPreview();     // the plane stays: it belongs to the world, not to the patch
        ++m_PatchVersion;
    }

    void CutController::ToggleEditPlane(const ModelInstance& instance)
    {
        m_PlaneEditing = !m_PlaneEditing;
        if (m_PlaneEditing)
        {
            if (!m_PlaneValid)
                PlacePlane(instance);
            m_PreviewDirty = true;
        }
        else
        {
            ClearPreview();
            m_CutRequested = false;
        }
    }

    void CutController::ToggleCap()
    {
        m_CapEnabled = !m_CapEnabled;
        m_PreviewDirty = true;      // the confirmed pieces keep the cap they were cut with
    }

    void CutController::ResetPlane(const ModelInstance& instance)
    {
        PlacePlane(instance);
    }

    void CutController::TranslatePlane(const Vector3& worldDelta)
    {
        if (!m_PlaneValid)
            return;
        Translate(m_Plane, { float(worldDelta.GetX()), float(worldDelta.GetY()), float(worldDelta.GetZ()) });
        m_PreviewDirty = true;
    }

    void CutController::MovePlaneAlongNormal(float distance)
    {
        if (!m_PlaneValid)
            return;
        const CutPlaneFrame frame = MakeFrame(m_Plane);
        Translate(m_Plane, { frame.normal.x * distance, frame.normal.y * distance, frame.normal.z * distance });
        m_PreviewDirty = true;
    }

    void CutController::RotatePlane(int localAxis, float radians)
    {
        if (!m_PlaneValid)
            return;
        RotateAroundLocalAxis(m_Plane, localAxis, radians);
        m_PreviewDirty = true;
    }

    void CutController::ScalePlane(float factor, bool uOnly)
    {
        if (!m_PlaneValid)
            return;
        if (uOnly)
            ScaleU(m_Plane, factor, MinExtent(), MaxExtent());
        else
            ScaleUniform(m_Plane, factor, MinExtent(), MaxExtent());
        m_PreviewDirty = true;
    }

    // Without a patch there is nothing to cut, so a request would only wait for the next C.
    void CutController::RequestCut()
    {
        if (m_PlaneEditing && m_PlaneValid && !m_PatchPiece.Empty())
            m_CutRequested = true;
    }

    // Only the confirmed pieces go. The patch stays, so it is on screen again; the preview resumes.
    void CutController::DiscardCut()
    {
        ClearSlice();
        m_PreviewDirty = true;
    }

    // Welds the current patch into the slicer input. An empty source (after X) just clears everything.
    void CutController::RebuildPatchPiece()
    {
        m_PatchPiece.Clear();
        m_WeldStats = WeldStats();
        m_PieceDetail.clear();
        m_PieceStatus = SliceStatus::InvalidInput;

        if (m_ExtractedMesh.GetData().indices.empty())
        {
            m_PieceDetail = "no patch";
            return;
        }

        WeldInput weldInput;
        std::string error;
        if (!MakeWeldInput(m_ExtractedMesh.GetData(), m_Source, weldInput, &error))
        {
            m_PieceDetail = error;
            return;
        }
        m_PieceStatus = BuildPatchPiece(weldInput, kDefaultEpsilonScale, m_PatchPiece, m_WeldStats, &m_PieceDetail);
        if (m_PieceStatus != SliceStatus::Ok)
            return;
        m_PatchRadius = float(ComputePieceRadius(m_PatchPiece));
    }

    // Runs once per frame after ModelInstance::Update. The patch is skinned to the current pose, the
    // slice plane (fixed in bone space) is rebuilt from the current joint matrices, and the cut pieces
    // are either cut again (when the plane controls changed) or skinned to the new pose.
    void CutController::Update(const Model& model, const ModelInstance& instance)
    {
        m_FramePoseValid = false;
        if (m_PatchPiece.Empty() || m_PieceStatus != SliceStatus::Ok)
            return;
        ScopedTimer _prof(L"Patch Pose Update");

        std::string error;
        if (!BuildSkinMatrices(model, instance, m_Source, m_SkinMatrices, &error)
            || !ComputePoseJointWorld(model, instance, m_Source, m_CurrentPoseJointWorld, &error))
        {
            m_SkinError = error;
            return;
        }

        int64_t skinTicks = 0;
        const auto skin = [&](PatchPiece& piece)
        {
            const int64_t start = SystemTime::GetCurrentTick();
            const bool ok = SkinPatchPiece(piece, m_SkinMatrices, nullptr, &error);
            skinTicks += SystemTime::GetCurrentTick() - start;
            return ok;
        };
        uint32_t skinnedNodes = 0;
        if (!skin(m_PatchPiece))
        {
            m_SkinError = error;
            return;
        }
        skinnedNodes += uint32_t(m_PatchPiece.nodes.size());


        // The preview is a dry-run of the current pose, so it is out of date when the animation moved.
        const size_t poseBytes = m_CurrentPoseJointWorld.size() * sizeof(Matrix4);
        if (m_PreviousPoseJointWorld.size() != m_CurrentPoseJointWorld.size()
            || memcmp(m_PreviousPoseJointWorld.data(), m_CurrentPoseJointWorld.data(), poseBytes) != 0)
        {
            m_PreviousPoseJointWorld = m_CurrentPoseJointWorld;
            m_PreviewDirty = true;
        }

        // One of: a confirmed cut (Enter), or a dry-run when something changed. Both read the patch
        // skinned just above, so new pieces start in this pose.
        bool freshlyCut = false;
        if (m_CutRequested)
        {
            m_CutRequested = false;
            if (m_PlaneValid)
                freshlyCut = ConfirmCut();      // a failed cut leaves the old pieces, which keep following below
        }
        else if (m_PlaneEditing && m_PreviewDirty && m_PlaneValid)
            RunPreview();
        if (m_HavePieces && !freshlyCut)
        {
            // Cut once, skinned every frame: same topology, new positions.
            PatchPiece* pieces[2] = { &m_SlicedPositive, &m_SlicedNegative };
            for (PatchPiece* piece : pieces)
            {
                if (!skin(*piece))
                {
                    m_SkinError = error;
                    return;
                }
                skinnedNodes += uint32_t(piece->nodes.size());
            }
        }
        if (freshlyCut)
            skinnedNodes += uint32_t(m_SlicedPositive.nodes.size() + m_SlicedNegative.nodes.size());

        m_SkinError.clear();
        m_SkinnedNodes = skinnedNodes;
        const float ms = float(SystemTime::TimeBetweenTicks(0, skinTicks) * 1000.0);
        m_SkinMs = m_SkinMs == 0.0f ? ms : m_SkinMs + (ms - m_SkinMs) * 0.1f;
        m_FramePoseValid = true;
    }

    // Puts the plane at the model's bounds center, normal world +Y (horizontal), half size radius / 2.
    void CutController::PlacePlane(const ModelInstance& instance)
    {
        const Vector3 center = instance.GetCenter();
        m_ModelRadius = float(instance.GetRadius());
        if (!(m_ModelRadius > 0.0f))
            m_ModelRadius = 1.0f;

        m_Plane = CutPlane();
        m_Plane.center = { float(center.GetX()), float(center.GetY()), float(center.GetZ()) };
        // Local +Z becomes world +Y: a quarter turn about X (Z -> -Y), so turn by -90 degrees.
        DirectX::XMStoreFloat4(&m_Plane.rotation, DirectX::XMQuaternionRotationAxis(DirectX::XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), -DirectX::XM_PIDIV2));
        m_Plane.halfExtents = { m_ModelRadius * 0.5f, m_ModelRadius * 0.5f };
        ClampExtents(m_Plane, MinExtent(), MaxExtent());
        m_PlaneValid = true;
        m_PreviewDirty = true;
    }

    void CutController::ClearSlice()
    {
        m_HavePieces = false;
        m_CutRequested = false;
        m_CutAttempted = false;
        m_SliceStatus = SliceStatus::InvalidInput;
        m_SliceDetail.clear();
        m_SliceInfo.clear();
        m_SlicedPositive.Clear();
        m_SlicedNegative.Clear();
        ++m_PieceGeneration;
    }

    void CutController::ClearPreview()
    {
        m_HavePreview = false;
        m_PreviewDirty = false;
        m_PreviewStatus = SliceStatus::InvalidInput;
        m_Preview = SliceResult();
        ++m_PreviewGeneration;
    }

    SliceOptions CutController::MakeSliceOptions() const
    {
        SliceOptions options;
        options.cap = m_CapEnabled ? CapMode::Create : CapMode::None;
        return options;
    }

    // The slicer cuts with the infinite plane; the rectangle only decides afterwards whether the cut is
    // accepted: every loop point must lie inside it (with a small margin). On failure only status and
    // detail are filled, like SlicePatch.
    SliceStatus CutController::SliceWithPlane(SliceResult& result)
    {
        const CutPlaneFrame frame = MakeFrame(m_Plane);
        const SliceStatus status = SlicePatch(m_PatchPiece, ToSlicePlane(frame), 1, MakeSliceOptions(), result);
        if (status != SliceStatus::Ok)
            return status;

        std::string detail;
        if (!LoopsInsidePlane(frame, result.positive, result.loops, m_ModelRadius * 0.01f, &detail))
        {
            result = SliceResult();
            result.status = SliceStatus::LoopOutsidePlane;
            result.detail = detail;
            return SliceStatus::LoopOutsidePlane;
        }
        return SliceStatus::Ok;
    }

    // Dry-run of the current plane on the uncut patch. The result is only kept to be shown; the
    // confirmed pieces are not touched.
    void CutController::RunPreview()
    {
        m_PreviewDirty = false;
        const int64_t start = SystemTime::GetCurrentTick();
        m_PreviewStatus = SliceWithPlane(m_Preview);
        const float ms = float(SystemTime::TimeBetweenTicks(start, SystemTime::GetCurrentTick()) * 1000.0);
        m_PreviewMs = m_PreviewMs == 0.0f ? ms : m_PreviewMs + (ms - m_PreviewMs) * 0.1f;
        m_HavePreview = true;
        ++m_PreviewGeneration;
    }

    // Cuts the uncut patch with the current plane (Enter). The pieces on screen are replaced only when
    // the slice is Ok; otherwise the old pieces stay and the status says why. Either way the request is used up.
    bool CutController::ConfirmCut()
    {
        m_CutAttempted = true;
        if (m_PieceStatus != SliceStatus::Ok)
        {
            m_SliceStatus = m_PieceStatus;
            m_SliceDetail = m_PieceDetail;
            m_SliceInfo.clear();
            return false;
        }
        const SlicePlane plane = CurrentPlane();

        SliceResult result;
        const int64_t start = SystemTime::GetCurrentTick();
        m_SliceStatus = SliceWithPlane(result);
        m_CutMs = float(SystemTime::TimeBetweenTicks(start, SystemTime::GetCurrentTick()) * 1000.0);
        m_SliceDetail = result.detail;
        if (m_SliceStatus != SliceStatus::Ok)
        {
            m_SliceInfo.clear();
            return false;
        }

        m_ShownNormal = plane.normal;
        m_HavePieces = true;
        m_SlicedPositive = result.positive;
        m_SlicedNegative = result.negative;
        ++m_PieceGeneration;
        ClearPreview();     // what the preview showed is now the result

        const double original = TotalArea(m_PatchPiece, FaceKind::Surface);
        const double positive = TotalArea(result.positive, FaceKind::Surface);
        const double negative = TotalArea(result.negative, FaceKind::Surface);
        wchar_t info[200];
        swprintf_s(info, L"loops %zu  + %zu faces %zu nodes   - %zu faces %zu nodes   area +%.2f -%.2f original %.2f (diff %.1e)",
            result.loops.size(), result.positive.faces.size(), result.positive.nodes.size(), result.negative.faces.size(),
            result.negative.nodes.size(), positive, negative, original, positive + negative - original);
        m_SliceInfo = info;
        return true;
    }
}
