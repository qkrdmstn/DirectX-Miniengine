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
            m_Patch.statusText = L"Leave cut mode (T) to copy a joint patch with C";
            return false;
        }

        // Copies with the pose of the last ModelInstance::Update, i.e. what is on screen now.
        ProceduralMeshData patch;
        BonePatchSource source;
        std::string error;
        if (ExtractBonePatch(model, instance, paletteId, jointIndex, 0.0f, patch, source, &error)
            && m_Patch.extracted.SetMesh(std::move(patch), &error))
        {
            m_Patch.source = std::move(source);
            m_Patch.statusText = L"Patch: " + std::to_wstring(m_Patch.extracted.GetData().indices.size() / 3) + L" tris, "
                + std::to_wstring(m_Patch.extracted.GetData().vertices.size()) + L" verts, mixed outside "
                + std::to_wstring(m_Patch.source.mixedOutsideTriangles);
            RebuildPatchPiece();
            ClearSlice();
            ClearPreview();
            m_Preview.previousPose.clear();
            m_Patch.poseJointWorld = m_Patch.source.poseJointWorld;
            ++m_Patch.version;
            return true;
        }
        m_Patch.statusText = L"Patch failed: " + Utility::UTF8ToWideString(error);
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
        m_Patch.extracted.Clear();
        m_Patch.source = {};
        m_Patch.statusText.clear();
        RebuildPatchPiece();
        ClearPreview();
        m_Preview.previousPose.clear();
        ++m_Patch.version;
    }

    void CutController::ToggleEditPlane(const ModelInstance& instance, uint32_t paletteId)
    {
        m_PlaneEditing = !m_PlaneEditing;
        m_CutRequested = false;
        DropPatch();    // a patch copied with C does not belong to the plane; the candidate patch replaces it
        m_Candidate.ResetSelection();
        m_Candidate.error.clear();
        m_Candidate.weightCycle = 0;
        if (m_PlaneEditing)
        {
            m_Candidate.palette = paletteId;
            if (!m_PlaneValid)
                PlacePlane(instance);
            m_Preview.dirty = true;
        }
    }

    void CutController::CycleWeightJoint()
    {
        if (!m_Candidate.cutJoints.empty())
            m_Candidate.weightCycle = (m_Candidate.weightCycle + 1) % uint32_t(m_Candidate.cutJoints.size());
    }

    uint32_t CutController::WeightJoint() const
    {
        if (!m_PlaneEditing || m_Candidate.cutJoints.empty())
            return kNoNode;
        return m_Candidate.cutJoints[m_Candidate.weightCycle % m_Candidate.cutJoints.size()];
    }

    void CutController::ToggleCap()
    {
        m_CapEnabled = !m_CapEnabled;
        m_Preview.dirty = true;      // the confirmed pieces keep the cap they were cut with
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
        m_Preview.dirty = true;
    }

    void CutController::MovePlaneAlongNormal(float distance)
    {
        if (!m_PlaneValid)
            return;
        const CutPlaneFrame frame = MakeFrame(m_Plane);
        Translate(m_Plane, { frame.normal.x * distance, frame.normal.y * distance, frame.normal.z * distance });
        m_Preview.dirty = true;
    }

    void CutController::RotatePlane(int localAxis, float radians)
    {
        if (!m_PlaneValid)
            return;
        RotateAroundLocalAxis(m_Plane, localAxis, radians);
        m_Preview.dirty = true;
    }

    void CutController::ScalePlane(float factor, bool uOnly)
    {
        if (!m_PlaneValid)
            return;
        if (uOnly)
            ScaleU(m_Plane, factor, MinExtent(), MaxExtent());
        else
            ScaleUniform(m_Plane, factor, MinExtent(), MaxExtent());
        m_Preview.dirty = true;
    }

    // Without a patch there is nothing to cut, so a request would only wait for the next C.
    void CutController::RequestCut()
    {
        if (m_PlaneEditing && m_PlaneValid && !m_Patch.piece.Empty())
            m_CutRequested = true;
    }

    // Only the confirmed pieces go. The patch stays, so it is on screen again; the preview resumes.
    void CutController::DiscardCut()
    {
        ClearSlice();
        m_Preview.dirty = true;
    }

    // Welds the current patch into the slicer input. An empty source (after X) just clears everything.
    void CutController::RebuildPatchPiece()
    {
        m_Patch.piece.Clear();
        m_Patch.weld = WeldStats();
        m_Patch.detail.clear();
        m_Patch.status = SliceStatus::InvalidInput;

        if (m_Patch.extracted.GetData().indices.empty())
        {
            m_Patch.detail = "no patch";
            return;
        }

        WeldInput weldInput;
        std::string error;
        if (!MakeWeldInput(m_Patch.extracted.GetData(), m_Patch.source, weldInput, &error))
        {
            m_Patch.detail = error;
            return;
        }
        m_Patch.status = BuildPatchPiece(weldInput, kDefaultEpsilonScale, m_Patch.piece, m_Patch.weld, &m_Patch.detail);
        if (m_Patch.status != SliceStatus::Ok)
            return;
        m_Patch.radius = float(ComputePieceRadius(m_Patch.piece));
    }

    // In cut mode: finds the bone segments the plane crosses and, when their parent joints (the cut joints)
    // are not the ones the current patch was extracted for, extracts and welds a new patch for them. The same
    // joint set is never extracted twice in a row, even after a failure; the patch is only skinned then.
    void CutController::UpdateCandidate(const Model& model, const ModelInstance& instance)
    {
        std::string error;
        if (!ComputePaletteSkeleton(model, instance, m_Candidate.palette, m_Candidate.skeleton, &error))
        {
            m_Candidate.skeleton = SkeletonPose();
            m_Candidate.error = Utility::UTF8ToWideString(error);
            // Without a valid selection the old candidate must not stay cuttable; the next good frame rebuilds it.
            DropPatch();
            m_Candidate.ResetSelection();
            return;
        }
        m_Candidate.error.clear();

        PickSegments(m_Candidate.skeleton, MakeFrame(m_Plane), m_Candidate.hits);
        m_Candidate.cutJoints = MeshCutting::CutJoints(m_Candidate.hits);
        m_Candidate.weightCycle = m_Candidate.cutJoints.empty() ? 0 : m_Candidate.weightCycle % uint32_t(m_Candidate.cutJoints.size());
        if (m_Candidate.haveAttempt && m_Candidate.cutJoints == m_Candidate.attemptJoints)
            return;
        m_Candidate.haveAttempt = true;
        m_Candidate.attemptJoints = m_Candidate.cutJoints;

        if (m_Candidate.cutJoints.empty())
        {
            DropPatch();
            m_Patch.statusText = L"No bone segment crosses the plane";
            return;
        }

        const int64_t start = SystemTime::GetCurrentTick();
        ProceduralMeshData patch;
        BonePatchSource source;
        if (ExtractBonePatch(model, instance, m_Candidate.palette, m_Candidate.cutJoints, 0.0f, patch, source, &error)
            && m_Patch.extracted.SetMesh(std::move(patch), &error))
        {
            m_Patch.source = std::move(source);
            RebuildPatchPiece();
            ClearPreview();     // the old dry-run was for another patch, even if this one failed to weld
            m_Patch.statusText = L"Candidate patch: " + std::to_wstring(m_Patch.extracted.GetData().indices.size() / 3) + L" tris, joints";
            for (uint32_t joint : m_Patch.source.joints)
                m_Patch.statusText += L" " + std::to_wstring(joint);
            m_Preview.previousPose.clear();
            m_Patch.poseJointWorld = m_Patch.source.poseJointWorld;
            m_Preview.dirty = true;
            ++m_Patch.version;
        }
        else
        {
            DropPatch();
            m_Patch.statusText = L"Candidate patch failed: " + Utility::UTF8ToWideString(error);
        }
        m_Candidate.extractMs = float(SystemTime::TimeBetweenTicks(start, SystemTime::GetCurrentTick()) * 1000.0);
        ++m_Candidate.extractCount;
    }

    // Runs once per frame after ModelInstance::Update. In cut mode the candidate patch follows the plane;
    // the patch is skinned to the current pose; a requested cut or the preview runs on it; the confirmed
    // pieces are skinned to the new pose (they stay when the patch is gone).
    void CutController::Update(const Model& model, const ModelInstance& instance)
    {
        m_FramePoseValid = false;
        if (m_PlaneEditing && m_PlaneValid)
            UpdateCandidate(model, instance);

        const bool havePatch = !m_Patch.piece.Empty() && m_Patch.status == SliceStatus::Ok;
        if (!havePatch)
            m_CutRequested = false;     // nothing to cut: do not keep the request for a later patch
        if (!havePatch && !m_Confirmed.have)
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
            if (!BuildSkinMatrices(model, instance, m_Patch.source, m_Patch.skinMatrices, &error)
                || !ComputePoseJointWorld(model, instance, m_Patch.source, m_Patch.poseJointWorld, &error))
            {
                m_SkinError = error;
                return;
            }
            if (!skin(m_Patch.piece, m_Patch.skinMatrices))
            {
                m_SkinError = error;
                return;
            }
            skinnedNodes += uint32_t(m_Patch.piece.nodes.size());

            // The preview is a dry-run of the current pose, so it is out of date when the animation moved.
            const size_t poseBytes = m_Patch.poseJointWorld.size() * sizeof(Matrix4);
            if (m_Preview.previousPose.size() != m_Patch.poseJointWorld.size()
                || memcmp(m_Preview.previousPose.data(), m_Patch.poseJointWorld.data(), poseBytes) != 0)
            {
                m_Preview.previousPose = m_Patch.poseJointWorld;
                m_Preview.dirty = true;
            }

            // One of: a confirmed cut (Enter), or a dry-run when something changed. Both read the patch
            // skinned just above, so new pieces start in this pose.
            if (m_CutRequested)
            {
                m_CutRequested = false;
                if (m_PlaneValid)
                    freshlyCut = ConfirmCut();      // a failed cut leaves the old pieces, which keep following below
            }
            else if (m_PlaneEditing && m_Preview.dirty && m_PlaneValid)
                RunPreview();
        }

        if (m_Confirmed.have && !freshlyCut)
        {
            // Cut once, skinned every frame: same topology, new positions. The pieces use the skin slots of
            // the patch they were cut from, which is not necessarily the current patch.
            if (!BuildSkinMatrices(model, instance, m_Confirmed.source, m_Confirmed.skinMatrices, &error))
            {
                m_SkinError = error;
                return;
            }
            PatchPiece* pieces[2] = { &m_Confirmed.positive, &m_Confirmed.negative };
            for (PatchPiece* piece : pieces)
            {
                if (!skin(*piece, m_Confirmed.skinMatrices))
                {
                    m_SkinError = error;
                    return;
                }
                skinnedNodes += uint32_t(piece->nodes.size());
            }
        }
        if (freshlyCut)
            skinnedNodes += uint32_t(m_Confirmed.positive.nodes.size() + m_Confirmed.negative.nodes.size());

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
        m_Preview.dirty = true;
    }

    void CutController::ClearSlice()
    {
        m_CutRequested = false;
        m_Confirmed.Clear();
    }

    void CutController::ClearPreview()
    {
        m_Preview.Clear();
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
        const SliceStatus status = SlicePatch(m_Patch.piece, ToSlicePlane(frame), 1, MakeSliceOptions(), result);
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
        m_Preview.dirty = false;
        const int64_t start = SystemTime::GetCurrentTick();
        m_Preview.status = SliceWithPlane(m_Preview.result);
        const float ms = float(SystemTime::TimeBetweenTicks(start, SystemTime::GetCurrentTick()) * 1000.0);
        m_Preview.ms = m_Preview.ms == 0.0f ? ms : m_Preview.ms + (ms - m_Preview.ms) * 0.1f;
        m_Preview.have = true;
        ++m_Preview.generation;
    }

    // Cuts the uncut patch with the current plane (Enter). The pieces on screen are replaced only when
    // the slice is Ok; otherwise the old pieces stay and the status says why. Either way the request is used up.
    bool CutController::ConfirmCut()
    {
        m_Confirmed.attempted = true;
        if (m_Patch.status != SliceStatus::Ok)
        {
            m_Confirmed.status = m_Patch.status;
            m_Confirmed.detail = m_Patch.detail;
            m_Confirmed.info.clear();
            return false;
        }
        const SlicePlane plane = CurrentPlane();

        SliceResult result;
        const int64_t start = SystemTime::GetCurrentTick();
        m_Confirmed.status = SliceWithPlane(result);
        m_Confirmed.ms = float(SystemTime::TimeBetweenTicks(start, SystemTime::GetCurrentTick()) * 1000.0);
        m_Confirmed.detail = result.detail;
        if (m_Confirmed.status != SliceStatus::Ok)
        {
            m_Confirmed.info.clear();
            return false;
        }

        m_Confirmed.shownNormal = plane.normal;
        m_Confirmed.have = true;
        // The pieces keep following the skin slots of this patch even after the candidate patch changes.
        m_Confirmed.source = BonePatchSource();
        m_Confirmed.source.paletteId = m_Patch.source.paletteId;
        m_Confirmed.source.jointIndex = m_Patch.source.jointIndex;
        m_Confirmed.source.joints = m_Patch.source.joints;
        m_Confirmed.source.meshes = m_Patch.source.meshes;
        m_Confirmed.positive = result.positive;
        m_Confirmed.negative = result.negative;
        ++m_Confirmed.generation;
        ClearPreview();     // what the preview showed is now the result

        const double original = TotalArea(m_Patch.piece, FaceKind::Surface);
        const double positive = TotalArea(result.positive, FaceKind::Surface);
        const double negative = TotalArea(result.negative, FaceKind::Surface);
        wchar_t info[200];
        swprintf_s(info, L"loops %zu  + %zu faces %zu nodes   - %zu faces %zu nodes   area +%.2f -%.2f original %.2f (diff %.1e)",
            result.loops.size(), result.positive.faces.size(), result.positive.nodes.size(), result.negative.faces.size(),
            result.negative.nodes.size(), positive, negative, original, positive + negative - original);
        m_Confirmed.info = info;
        return true;
    }
}
