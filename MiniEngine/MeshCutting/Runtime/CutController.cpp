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
        if (m_PlaneEditing)
        {
            m_PatchStatusText = L"Leave cut mode (T) to copy a joint patch with C";
            return false;
        }

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
            ++m_PatchVersion;
            return true;
        }
        m_PatchStatusText = L"Patch failed: " + Utility::UTF8ToWideString(error);
        return false;
    }

    // X: the patch and the confirmed pieces. Not available in cut mode, where the plane decides the patch.
    void CutController::ClearPatch()
    {
        if (m_PlaneEditing)
            return;
        DropPatch();
        ClearSlice();
    }

    // The patch only. The confirmed pieces and the plane stay.
    void CutController::DropPatch()
    {
        m_ExtractedMesh.Clear();
        m_Source = {};
        m_PatchStatusText.clear();
        RebuildPatchPiece();
        ClearPreview();
        m_PreviousPoseJointWorld.clear();
        ++m_PatchVersion;
    }

    void CutController::ToggleEditPlane(const ModelInstance& instance, uint32_t paletteId)
    {
        m_PlaneEditing = !m_PlaneEditing;
        m_CutRequested = false;
        DropPatch();    // a patch copied with C does not belong to the plane; the candidate patch replaces it
        m_HaveAttempt = false;
        m_AttemptJoints.clear();
        m_Hits.clear();
        m_CutJoints.clear();
        m_CandidateError.clear();
        m_WeightCycle = 0;
        if (m_PlaneEditing)
        {
            m_EditPalette = paletteId;
            if (!m_PlaneValid)
                PlacePlane(instance);
            m_PreviewDirty = true;
        }
    }

    void CutController::CycleWeightJoint()
    {
        if (!m_CutJoints.empty())
            m_WeightCycle = (m_WeightCycle + 1) % uint32_t(m_CutJoints.size());
    }

    uint32_t CutController::WeightJoint() const
    {
        if (!m_PlaneEditing || m_CutJoints.empty())
            return kNoNode;
        return m_CutJoints[m_WeightCycle % m_CutJoints.size()];
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

    // In cut mode: finds the bone segments the plane crosses and, when their parent joints (the cut joints)
    // are not the ones the current patch was extracted for, extracts and welds a new patch for them. The same
    // joint set is never extracted twice in a row, even after a failure; the patch is only skinned then.
    void CutController::UpdateCandidate(const Model& model, const ModelInstance& instance)
    {
        std::string error;
        if (!ComputePaletteSkeleton(model, instance, m_EditPalette, m_Skeleton, &error))
        {
            m_Skeleton = SkeletonPose();
            m_Hits.clear();
            m_CutJoints.clear();
            m_CandidateError = Utility::UTF8ToWideString(error);
            // Without a valid selection the old candidate must not stay cuttable; the next good frame rebuilds it.
            DropPatch();
            m_HaveAttempt = false;
            m_AttemptJoints.clear();
            return;
        }
        m_CandidateError.clear();

        PickSegments(m_Skeleton, MakeFrame(m_Plane), m_Hits);
        m_CutJoints = MeshCutting::CutJoints(m_Hits);
        m_WeightCycle = m_CutJoints.empty() ? 0 : m_WeightCycle % uint32_t(m_CutJoints.size());
        if (m_HaveAttempt && m_CutJoints == m_AttemptJoints)
            return;
        m_HaveAttempt = true;
        m_AttemptJoints = m_CutJoints;

        if (m_CutJoints.empty())
        {
            DropPatch();
            m_PatchStatusText = L"No bone segment crosses the plane";
            return;
        }

        const int64_t start = SystemTime::GetCurrentTick();
        ProceduralMeshData patch;
        BonePatchSource source;
        if (ExtractBonePatch(model, instance, m_EditPalette, m_CutJoints, 0.0f, patch, source, &error)
            && m_ExtractedMesh.SetMesh(std::move(patch), &error))
        {
            m_Source = std::move(source);
            RebuildPatchPiece();
            ClearPreview();     // the old dry-run was for another patch, even if this one failed to weld
            m_PatchStatusText = L"Candidate patch: " + std::to_wstring(m_ExtractedMesh.GetData().indices.size() / 3) + L" tris, joints";
            for (uint32_t joint : m_Source.joints)
                m_PatchStatusText += L" " + std::to_wstring(joint);
            m_PreviousPoseJointWorld.clear();
            m_CurrentPoseJointWorld = m_Source.poseJointWorld;
            m_PreviewDirty = true;
            ++m_PatchVersion;
        }
        else
        {
            DropPatch();
            m_PatchStatusText = L"Candidate patch failed: " + Utility::UTF8ToWideString(error);
        }
        m_ExtractMs = float(SystemTime::TimeBetweenTicks(start, SystemTime::GetCurrentTick()) * 1000.0);
        ++m_ExtractCount;
    }

    // Runs once per frame after ModelInstance::Update. In cut mode the candidate patch follows the plane;
    // the patch is skinned to the current pose; a requested cut or the preview runs on it; the confirmed
    // pieces are skinned to the new pose (they stay when the patch is gone).
    void CutController::Update(const Model& model, const ModelInstance& instance)
    {
        m_FramePoseValid = false;
        if (m_PlaneEditing && m_PlaneValid)
            UpdateCandidate(model, instance);

        const bool havePatch = !m_PatchPiece.Empty() && m_PieceStatus == SliceStatus::Ok;
        if (!havePatch)
            m_CutRequested = false;     // nothing to cut: do not keep the request for a later patch
        if (!havePatch && !m_HavePieces)
            return;
        ScopedTimer _prof(L"Patch Pose Update");

        std::string error;
        int64_t skinTicks = 0;
        const auto skin = [&](PatchPiece& piece, const std::vector<std::vector<DirectX::XMFLOAT4X4>>& matrices)
        {
            const int64_t start = SystemTime::GetCurrentTick();
            const bool ok = SkinPatchPiece(piece, matrices, nullptr, &error);
            skinTicks += SystemTime::GetCurrentTick() - start;
            return ok;
        };
        uint32_t skinnedNodes = 0;
        bool freshlyCut = false;

        if (havePatch)
        {
            if (!BuildSkinMatrices(model, instance, m_Source, m_SkinMatrices, &error)
                || !ComputePoseJointWorld(model, instance, m_Source, m_CurrentPoseJointWorld, &error))
            {
                m_SkinError = error;
                return;
            }
            if (!skin(m_PatchPiece, m_SkinMatrices))
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
            if (m_CutRequested)
            {
                m_CutRequested = false;
                if (m_PlaneValid)
                    freshlyCut = ConfirmCut();      // a failed cut leaves the old pieces, which keep following below
            }
            else if (m_PlaneEditing && m_PreviewDirty && m_PlaneValid)
                RunPreview();
        }

        if (m_HavePieces && !freshlyCut)
        {
            // Cut once, skinned every frame: same topology, new positions. The pieces use the skin slots of
            // the patch they were cut from, which is not necessarily the current patch.
            if (!BuildSkinMatrices(model, instance, m_PieceSource, m_PieceSkinMatrices, &error))
            {
                m_SkinError = error;
                return;
            }
            PatchPiece* pieces[2] = { &m_SlicedPositive, &m_SlicedNegative };
            for (PatchPiece* piece : pieces)
            {
                if (!skin(*piece, m_PieceSkinMatrices))
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
        // The pieces keep following the skin slots of this patch even after the candidate patch changes.
        m_PieceSource = BonePatchSource();
        m_PieceSource.paletteId = m_Source.paletteId;
        m_PieceSource.jointIndex = m_Source.jointIndex;
        m_PieceSource.joints = m_Source.joints;
        m_PieceSource.meshes = m_Source.meshes;
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
