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
            m_PlaneOffset = m_PlaneTiltA = m_PlaneTiltB = 0.0f;
            ComputeBasePlane(model);
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
        m_PlaneBaseValid = false;
        m_PlaneEditing = false;
        ClearPreview();
        ++m_PatchVersion;
    }

    void CutController::ToggleEditPlane()
    {
        m_PlaneEditing = !m_PlaneEditing;
        if (m_PlaneEditing)
            m_PreviewDirty = true;
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

    void CutController::ResetPlane()
    {
        m_PlaneOffset = m_PlaneTiltA = m_PlaneTiltB = 0.0f;
        m_PreviewDirty = true;
    }

    void CutController::NudgePlane(float offset, float tiltA, float tiltB)
    {
        m_PlaneOffset += offset;
        m_PlaneTiltA += tiltA;
        m_PlaneTiltB += tiltB;
        m_PreviewDirty = true;
    }

    void CutController::RequestCut()
    {
        if (m_PlaneEditing && m_PlaneBaseValid)
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

        // The plane is fixed in the selected bone's local space: carry it to this frame's joint matrix.
        UpdateBasePlane();

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
            if (m_PlaneBaseValid)
                freshlyCut = ConfirmCut();      // a failed cut leaves the old pieces, which keep following below
        }
        else if (m_PlaneEditing && m_PreviewDirty && m_PlaneBaseValid)
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

    // The plane starts at the selected joint and points along the bone to its child joint (or away
    // from its parent for a leaf joint), measured in the joint matrices of the current pose.
    void CutController::ComputeBasePlane(const Model& model)
    {
        m_PlaneBaseValid = false;
        const std::vector<Matrix4>& pose = m_CurrentPoseJointWorld;
        if (m_Source.paletteId >= model.m_SkinPalettes.size() || m_Source.jointIndex >= pose.size())
            return;
        const SkinPalette& palette = model.m_SkinPalettes[m_Source.paletteId];
        const uint32_t joint = m_Source.jointIndex;
        const uint32_t node = model.m_JointIndices[palette.startJoint + joint];
        const Vector3 origin = Vector3(pose[joint].GetW());

        Vector3 direction(kZero);
        bool found = false;
        for (uint32_t j = 0; j < palette.numJoints && !found; ++j)
        {
            const uint32_t other = model.m_JointIndices[palette.startJoint + j];
            if (other < model.m_NodeParents.size() && model.m_NodeParents[other] == node)
            {
                direction = Vector3(pose[j].GetW()) - origin;
                found = true;
            }
        }
        for (uint32_t j = 0; j < palette.numJoints && !found && node < model.m_NodeParents.size(); ++j)
            if (model.m_JointIndices[palette.startJoint + j] == model.m_NodeParents[node])
            {
                direction = origin - Vector3(pose[j].GetW());
                found = true;
            }
        if (!found || float(Length(direction)) < 1e-5f)
            direction = Vector3(0.0f, 1.0f, 0.0f);
        direction = Normalize(direction);

        Vector3 up(0.0f, 1.0f, 0.0f);
        if (fabsf(float(Dot(direction, up))) > 0.99f)
            up = Vector3(1.0f, 0.0f, 0.0f);
        const Vector3 axisA = Normalize(Cross(direction, up));
        const Vector3 axisB = Cross(direction, axisA);

        // Keep the frame in the joint's local space; UpdateBasePlane carries it to each frame's pose.
        const Matrix4 toLocal = Invert(pose[joint]);
        const Matrix3 toLocal3 = toLocal.Get3x3();
        m_PlaneLocalPoint = Vector3(toLocal * origin);
        // The normal is a covector (it must stay perpendicular to the plane under non-uniform scale):
        // it goes local with the transpose and comes back with the inverse transpose.
        m_PlaneLocalNormal = Normalize(Transpose(pose[joint].Get3x3()) * direction);
        m_PlaneLocalAxisA = Normalize(toLocal3 * axisA);
        m_PlaneLocalAxisB = Normalize(toLocal3 * axisB);
        m_PlaneBaseValid = true;
        UpdateBasePlane();
    }

    // Carries the stored bone-local plane frame to the current pose of the selected joint.
    void CutController::UpdateBasePlane()
    {
        const uint32_t joint = m_Source.jointIndex;
        if (!m_PlaneBaseValid || joint >= m_CurrentPoseJointWorld.size())
            return;
        const Matrix4& world = m_CurrentPoseJointWorld[joint];
        const Matrix3 world3 = world.Get3x3();
        m_PlanePoint0 = Vector3(world * m_PlaneLocalPoint);
        m_PlaneNormal0 = Normalize(InverseTranspose(world3) * m_PlaneLocalNormal);
        m_PlaneAxisA = Normalize(world3 * m_PlaneLocalAxisA);
        m_PlaneAxisB = Normalize(world3 * m_PlaneLocalAxisB);
    }

    SlicePlane CutController::CurrentPlane() const
    {
        const Vector3 normal = Normalize(Quaternion(m_PlaneAxisB, m_PlaneTiltB) * (Quaternion(m_PlaneAxisA, m_PlaneTiltA) * m_PlaneNormal0));
        const Vector3 point = m_PlanePoint0 + m_PlaneNormal0 * m_PlaneOffset;
        SlicePlane plane;
        plane.point = { float(point.GetX()), float(point.GetY()), float(point.GetZ()) };
        plane.normal = { float(normal.GetX()), float(normal.GetY()), float(normal.GetZ()) };
        return plane;
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

    // Dry-run of the current plane on the uncut patch. The result is only kept to be shown; the
    // confirmed pieces are not touched.
    void CutController::RunPreview()
    {
        m_PreviewDirty = false;
        const int64_t start = SystemTime::GetCurrentTick();
        m_PreviewStatus = SlicePatch(m_PatchPiece, CurrentPlane(), 1, MakeSliceOptions(), m_Preview);
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
        m_SliceStatus = SlicePatch(m_PatchPiece, plane, 1, MakeSliceOptions(), result);
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
