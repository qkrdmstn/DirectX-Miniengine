#pragma once

#include "../BonePatch.h"
#include "../ProceduralMesh.h"
#include "../Geometry/CutPlane.h"
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
    // State and logic of the cutting demo, without any drawing: the copied patch, the cut plane
    // (a finite rectangle fixed in world space, see Docs/PlaneFirstCutDesign.md), the cut pieces and
    // the per-frame skinning that keeps the patch and the pieces on the pose the GPU draws.
    // CutDebugView draws what is exposed here.
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
        // The first T (or R) puts the plane at the model's bounds center, normal +Y, half size = radius / 2.
        // Cut mode: the patch comes from the plane (the bones it crosses), not from J/K + C. Entering drops a
        // patch copied with C; leaving drops the candidate patch. The confirmed pieces stay in both cases.
        void ToggleEditPlane(const ModelInstance& instance, uint32_t paletteId);   // T
        void CycleWeightJoint();                                    // Tab: which cut joint the weight view shows
        void ToggleCap();                                           // M: applies to the preview and the next cut
        void ResetPlane(const ModelInstance& instance);             // R
        void TranslatePlane(const Math::Vector3& worldDelta);       // Home/End/Delete/PgDn (camera axes)
        void MovePlaneAlongNormal(float distance);                  // [ ]
        void RotatePlane(int localAxis, float radians);             // , . ; ' Insert PgUp  (0 = U, 1 = V, 2 = normal)
        void ScalePlane(float factor, bool uOnly);                  // - =  (Shift: U only)
        void RequestCut();                                          // Enter: one cut in the next Update (plane edit only)
        void DiscardCut();                                          // Backspace: drop the confirmed pieces

        // Once per frame, right after ModelInstance::Update: skins the patch and the pieces to the
        // current pose, then runs the requested cut or the preview. The plane stays where it is in world space.
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

        bool PlaneValid() const { return m_PlaneValid; }            // the plane has been placed (first T or R)
        bool PlaneEditing() const { return m_PlaneEditing; }
        bool CapEnabled() const { return m_CapEnabled; }
        const CutPlane& Plane() const { return m_Plane; }
        CutPlaneFrame PlaneFrame() const { return MakeFrame(m_Plane); }
        SlicePlane CurrentPlane() const { return ToSlicePlane(MakeFrame(m_Plane)); }
        float ModelRadius() const { return m_ModelRadius; }         // scale of the plane controls and debug tubes

        // The candidate: what the plane crosses and the patch built from it (cut mode only).
        uint32_t EditPalette() const { return m_EditPalette; }      // the palette the cut mode works on (chosen when T was pressed)
        const SkeletonPose& Skeleton() const { return m_Skeleton; }        // the edited palette at the current pose
        const std::vector<SegmentHit>& Hits() const { return m_Hits; }     // bone segments the plane crosses
        const std::vector<uint32_t>& CutJoints() const { return m_CutJoints; }   // parent joints of the hits
        bool NoSegment() const { return m_PlaneEditing && m_PlaneValid && m_Hits.empty(); }
        uint32_t WeightJoint() const;                               // cut joint for the weight view, kNoNode if none
        const std::wstring& CandidateError() const { return m_CandidateError; }
        float ExtractMs() const { return m_ExtractMs; }             // last candidate extraction + weld
        uint32_t ExtractCount() const { return m_ExtractCount; }    // how many candidate extractions ran

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
        void DropPatch();               // the patch only (not the confirmed pieces)
        void UpdateCandidate(const Model& model, const ModelInstance& instance);
        void PlacePlane(const ModelInstance& instance);
        float MinExtent() const { return m_ModelRadius * 0.01f; }
        float MaxExtent() const { return m_ModelRadius * 3.0f; }
        // SlicePatch with the current plane, then the finite-rectangle check on the cut loops.
        SliceStatus SliceWithPlane(SliceResult& result);
        bool ConfirmCut();             // true when new pieces replaced the old ones
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

        // cut plane: a finite rectangle in world space (the space of the posed patch), tied to no joint
        bool m_PlaneEditing = false;
        bool m_PreviewDirty = false;    // plane, cap, patch or pose changed: the dry-run is out of date
        bool m_CutRequested = false;    // Enter: one confirmed cut in the next Update
        bool m_CapEnabled = true;
        bool m_PlaneValid = false;
        CutPlane m_Plane;
        float m_ModelRadius = 0.0f;     // of the instance when the plane was placed

        // cut result: the pieces on screen come from the last Ok slice and are skinned every frame
        SliceStatus m_SliceStatus = SliceStatus::InvalidInput;
        std::string m_SliceDetail;
        std::wstring m_SliceInfo;
        bool m_HavePieces = false;
        DirectX::XMFLOAT3 m_ShownNormal = { 0.0f, 1.0f, 0.0f };
        PatchPiece m_SlicedPositive, m_SlicedNegative;
        uint64_t m_PieceGeneration = 0;
        bool m_CutAttempted = false;
        BonePatchSource m_PieceSource;  // meshes of the patch the pieces were cut from (their skin slots)
        std::vector<std::vector<DirectX::XMFLOAT4X4>> m_PieceSkinMatrices;
        float m_CutMs = 0.0f;

        // preview: a dry-run of the plane on the uncut patch, kept only to be shown
        SliceResult m_Preview;
        bool m_HavePreview = false;
        SliceStatus m_PreviewStatus = SliceStatus::InvalidInput;
        uint64_t m_PreviewGeneration = 0;
        float m_PreviewMs = 0.0f;
        std::vector<Math::Matrix4> m_PreviousPoseJointWorld;    // to notice that the animation moved

        // candidate (cut mode): bone segments crossed by the plane and the patch extracted for their parent joints
        uint32_t m_EditPalette = 0;
        SkeletonPose m_Skeleton;
        std::vector<SegmentHit> m_Hits;
        std::vector<uint32_t> m_CutJoints;
        std::vector<uint32_t> m_AttemptJoints;  // the last joint set an extraction was tried for
        bool m_HaveAttempt = false;
        uint32_t m_WeightCycle = 0;
        std::wstring m_CandidateError;
        float m_ExtractMs = 0.0f;
        uint32_t m_ExtractCount = 0;

        // animation follow-up
        std::vector<std::vector<DirectX::XMFLOAT4X4>> m_SkinMatrices;
        std::vector<Math::Matrix4> m_CurrentPoseJointWorld;
        uint32_t m_SkinnedNodes = 0;    // nodes skinned in the last frame (patch + pieces)
        float m_SkinMs = 0.0f;          // smoothed CPU time of the SkinPatchPiece calls
        std::string m_SkinError;
        bool m_FramePoseValid = false;
    };
}
