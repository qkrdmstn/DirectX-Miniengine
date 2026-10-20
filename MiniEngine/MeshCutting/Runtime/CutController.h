#pragma once

#include "../BonePatch.h"
#include "../ProceduralMesh.h"
#include "../Geometry/PatchGeometry.h"
#include "../Geometry/PatchWeld.h"
#include "../Geometry/PieceSkinning.h"
#include "../Geometry/PlaneSlicer.h"
#include "VectorMath.h"
#include <cstdint>
#include <string>
#include <vector>

class Model;
class ModelInstance;

namespace MeshCutting
{
    // State and logic of the cutting demo, without any drawing: the copied patch, the slice plane
    // (fixed in the selected bone's local space), the cut pieces and the per-frame skinning that
    // keeps all of them on the pose the GPU draws. CutDebugView draws what is exposed here.
    class CutController
    {
    public:
        // ---- patch (C / X) ----
        // Copies the triangles of one joint with the pose of the last ModelInstance::Update. On
        // failure the previous patch stays and PatchStatusText says why.
        bool SetPatch(const Model& model, const ModelInstance& instance, uint32_t paletteId, uint32_t jointIndex);
        void ClearPatch();

        // ---- plane controls and cutting ----
        // Moving the plane only changes the preview (a dry-run on the uncut patch). The confirmed
        // pieces change only on RequestCut, or are dropped by DiscardCut.
        void ToggleEditPlane();                                     // T: plane edit on/off (the result stays)
        void ToggleCap();                                           // M: applies to the preview and the next cut
        void ResetPlane();                                          // R
        void NudgePlane(float offset, float tiltA, float tiltB);    // [ ] , . ; '  (amounts are deltas)
        void RequestCut();                                          // Enter: one cut in the next Update (plane edit only)
        void DiscardCut();                                          // Backspace: drop the confirmed pieces

        // Once per frame, right after ModelInstance::Update: skins the patch and the pieces to the
        // current pose, moves the plane with the bone, then runs the requested cut or the preview.
        void Update(const Model& model, const ModelInstance& instance);

        // ---- read-only state for the view and the UI ----
        const ProceduralMesh& ExtractedMesh() const { return m_ExtractedMesh; }    // patch as extracted (CPU copy)
        const PatchPiece& Patch() const { return m_PatchPiece; }
        const WeldStats& Weld() const { return m_WeldStats; }
        SliceStatus PatchStatus() const { return m_PieceStatus; }
        const std::string& PatchDetail() const { return m_PieceDetail; }
        const std::wstring& PatchStatusText() const { return m_PatchStatusText; }
        uint64_t PatchVersion() const { return m_PatchVersion; }    // changes on C and X
        float PatchRadius() const { return m_PatchRadius; }

        bool PlaneBaseValid() const { return m_PlaneBaseValid; }
        bool PlaneEditing() const { return m_PlaneEditing; }
        bool CapEnabled() const { return m_CapEnabled; }
        float PlaneOffset() const { return m_PlaneOffset; }
        float PlaneTiltA() const { return m_PlaneTiltA; }
        float PlaneTiltB() const { return m_PlaneTiltB; }
        SlicePlane CurrentPlane() const;

        bool HasPieces() const { return m_HavePieces; }
        const PatchPiece& SlicedPositive() const { return m_SlicedPositive; }
        const PatchPiece& SlicedNegative() const { return m_SlicedNegative; }
        uint64_t PieceGeneration() const { return m_PieceGeneration; }  // changes when the pieces are replaced or cleared
        const DirectX::XMFLOAT3& ShownNormal() const { return m_ShownNormal; }
        bool CutAttempted() const { return m_CutAttempted; }       // Enter was handled since the patch was copied
        SliceStatus LastSliceStatus() const { return m_SliceStatus; }   // of the last confirmed cut attempt
        const std::string& LastSliceDetail() const { return m_SliceDetail; }
        const std::wstring& SliceInfo() const { return m_SliceInfo; }
        float CutMs() const { return m_CutMs; }                    // time of the last confirmed SlicePatch

        // The dry-run of the current plane. Its pieces are never drawn; only its loops are.
        bool HasPreview() const { return m_HavePreview; }          // a dry-run ran for the current plane
        const SliceResult& Preview() const { return m_Preview; }   // loops/pieces are filled only when PreviewStatus() is Ok
        SliceStatus PreviewStatus() const { return m_PreviewStatus; }
        uint64_t PreviewGeneration() const { return m_PreviewGeneration; }  // changes on every dry-run and every clear
        float PreviewMs() const { return m_PreviewMs; }            // smoothed time of the dry-run SlicePatch

        bool FramePoseValid() const { return m_FramePoseValid; }    // the last Update reached the end
        uint32_t SkinnedNodes() const { return m_SkinnedNodes; }
        float SkinMs() const { return m_SkinMs; }
        const std::string& SkinError() const { return m_SkinError; }

    private:
        void RebuildPatchPiece();
        void ComputeBasePlane(const Model& model);
        void UpdateBasePlane();
        bool ConfirmCut();              // true when new pieces replaced the old ones
        void RunPreview();
        void ClearSlice();              // drops the confirmed pieces (and any pending cut request)
        void ClearPreview();
        SliceOptions MakeSliceOptions() const;

        // patch
        ProceduralMesh m_ExtractedMesh;
        BonePatchSource m_Source;
        std::wstring m_PatchStatusText;
        PatchPiece m_PatchPiece;        // welded copy of the patch: the slicer input
        WeldStats m_WeldStats;
        SliceStatus m_PieceStatus = SliceStatus::InvalidInput;
        std::string m_PieceDetail;
        uint64_t m_PatchVersion = 0;
        float m_PatchRadius = 0.0f;     // bounds radius when the patch was copied (keeps debug sizes stable)

        // slice plane: the frame is stored in the selected joint's local space (copy time) and carried
        // to the world with the joint's current matrix every frame, so it follows the bone, twist included
        bool m_PlaneEditing = false;
        bool m_PreviewDirty = false;    // plane, cap, patch or pose changed: the dry-run is out of date
        bool m_CutRequested = false;    // Enter: one confirmed cut in the next Update
        bool m_CapEnabled = true;
        bool m_PlaneBaseValid = false;
        Math::Vector3 m_PlaneLocalPoint, m_PlaneLocalNormal, m_PlaneLocalAxisA, m_PlaneLocalAxisB;
        Math::Vector3 m_PlanePoint0, m_PlaneNormal0, m_PlaneAxisA, m_PlaneAxisB;    // world frame of this frame
        float m_PlaneOffset = 0.0f;
        float m_PlaneTiltA = 0.0f;
        float m_PlaneTiltB = 0.0f;

        // cut result: the pieces on screen come from the last Ok slice and are skinned every frame
        SliceStatus m_SliceStatus = SliceStatus::InvalidInput;
        std::string m_SliceDetail;
        std::wstring m_SliceInfo;
        bool m_HavePieces = false;
        DirectX::XMFLOAT3 m_ShownNormal = { 0.0f, 1.0f, 0.0f };
        PatchPiece m_SlicedPositive, m_SlicedNegative;
        uint64_t m_PieceGeneration = 0;
        bool m_CutAttempted = false;
        float m_CutMs = 0.0f;

        // preview: a dry-run of the plane on the uncut patch, kept only to be shown
        SliceResult m_Preview;
        bool m_HavePreview = false;
        SliceStatus m_PreviewStatus = SliceStatus::InvalidInput;
        uint64_t m_PreviewGeneration = 0;
        float m_PreviewMs = 0.0f;
        std::vector<Math::Matrix4> m_PreviousPoseJointWorld;    // to notice that the animation moved

        // animation follow-up
        std::vector<std::vector<DirectX::XMFLOAT4X4>> m_SkinMatrices;
        std::vector<Math::Matrix4> m_CurrentPoseJointWorld;
        uint32_t m_SkinnedNodes = 0;    // nodes skinned in the last frame (patch + pieces)
        float m_SkinMs = 0.0f;          // smoothed CPU time of the SkinPatchPiece calls
        std::string m_SkinError;
        bool m_FramePoseValid = false;
    };
}
