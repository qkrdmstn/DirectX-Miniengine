#pragma once

#include "../BonePatch.h"
#include "../ProceduralMesh.h"
#include "../Geometry/CutPlane.h"
#include "../Geometry/Patch.h"
#include "../Geometry/Skinning.h"
#include "../Geometry/Slice.h"
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
        // ---- debug mode and cutting ----
        // Moving the plane only changes the preview (a dry-run on the uncut patch). The confirmed
        // pieces change only on RequestCut, or are dropped by DiscardCut.
        // The first T puts the plane at the model's bounds center, normal +Y, half size = radius / 2.
        // In debug mode the patch comes from the plane (the bones it crosses). Leaving debug mode drops the
        // candidate patch; the confirmed pieces and the plane stay.
        void ToggleEditPlane(const ModelInstance& instance, uint32_t paletteId);   // T
        // The plane owns the gizmo (tool, hover, drag). A drag that moves, rotates or scales the plane marks the preview out of date.
        void SetGizmoTool(GizmoTool tool) { m_Plane.SetGizmoTool(tool); }
        bool UpdateGizmo(const GizmoView& view, const GizmoInput& input);   // true when the drag changed the plane
        void CancelGizmo() { m_Plane.CancelGizmo(); }
        void RequestCut();                      // Enter: one cut in the next Update (debug mode only)
        void DiscardCut();                      // Ctrl+Z: drop the confirmed pieces

        // Once per frame, right after ModelInstance::Update: skins the patch and the pieces to the
        // current pose, then runs the requested cut or the preview. The plane stays where it is in world space.
        void Update(const Model& model, const ModelInstance& instance);

        // ---- read-only state for the view and the UI ----
        const PatchPiece& Patch() const { return m_Patch.piece; }
        SliceStatus PatchStatus() const { return m_Patch.status; }
        const std::string& PatchDetail() const { return m_Patch.detail; }
        const std::wstring& PatchStatusText() const { return m_Patch.statusText; }
        float PatchRadius() const { return m_Patch.radius; }

        bool PlaneValid() const { return m_PlaneValid; }            // the plane has been placed (first T)
        bool PlaneEditing() const { return m_PlaneEditing; }
        const CutPlane& GetPlane() const { return m_Plane; }
        float ModelRadius() const { return m_ModelRadius; }         // scale of the plane controls and debug tubes

        // The candidate: what the plane crosses and the patch built from it (cut mode only).
        uint32_t EditPalette() const { return m_Candidate.palette; }      // the palette the cut mode works on (chosen when T was pressed)
        const SkeletonPose& Skeleton() const { return m_Candidate.skeleton; }        // the edited palette at the current pose
        const std::vector<SegmentHit>& Hits() const { return m_Candidate.hits; }     // bone segments the plane crosses
        const std::vector<uint32_t>& CutJoints() const { return m_Candidate.cutJoints; }   // parent joints of the hits
        bool NoSegment() const { return m_PlaneEditing && m_PlaneValid && m_Candidate.hits.empty(); }
        uint32_t WeightJoint() const;                               // the first cut joint, for the weight view; kNoNode if none
        const std::wstring& CandidateError() const { return m_Candidate.error; }
        float ExtractMs() const { return m_Candidate.extractMs; }             // last candidate extraction + weld
        uint32_t ExtractCount() const { return m_Candidate.extractCount; }    // how many candidate extractions ran

        bool HasPieces() const { return m_Confirmed.have; }
        const PatchPiece& SlicedPositive() const { return m_Confirmed.positive; }
        const PatchPiece& SlicedNegative() const { return m_Confirmed.negative; }
        uint64_t PieceGeneration() const { return m_Confirmed.generation; }  // changes when the pieces are replaced or cleared
        const DirectX::XMFLOAT3& ShownNormal() const { return m_Confirmed.shownNormal; }
        bool CutAttempted() const { return m_Confirmed.attempted; }       // Enter was handled since the last undo
        SliceStatus LastSliceStatus() const { return m_Confirmed.status; }   // of the last confirmed cut attempt
        const std::string& LastSliceDetail() const { return m_Confirmed.detail; }
        const std::wstring& SliceInfo() const { return m_Confirmed.info; }
        float CutMs() const { return m_Confirmed.ms; }                    // time of the last confirmed SlicePatch

        // The dry-run of the current plane. Its pieces are never drawn; only its loops are.
        bool HasPreview() const { return m_Preview.have; }          // a dry-run ran for the current plane
        const SliceResult& Preview() const { return m_Preview.result; }   // loops/pieces are filled only when PreviewStatus() is Ok
        SliceStatus PreviewStatus() const { return m_Preview.status; }
        uint64_t PreviewGeneration() const { return m_Preview.generation; }  // changes on every dry-run and every clear
        float PreviewMs() const { return m_Preview.ms; }            // smoothed time of the dry-run SlicePatch

        bool FramePoseValid() const { return m_FramePoseValid; }    // the last Update reached the end
        uint32_t SkinnedNodes() const { return m_SkinnedNodes; }
        float SkinMs() const { return m_SkinMs; }
        const std::string& SkinError() const { return m_SkinError; }

    private:
        void RebuildPatchPiece();
        void DropPatch();               // the patch only (not the confirmed pieces)
        void UpdateCandidate(const Model& model, const ModelInstance& instance);
        void PlacePlane(const ModelInstance& instance);
        // SlicePatch with the current plane, then the finite-rectangle check on the cut loops.
        SliceStatus SliceWithPlane(SliceResult& result);
        bool ConfirmCut();             // true when new pieces replaced the old ones
        void RunPreview();
        void ClearSlice();              // drops the confirmed pieces (and any pending cut request)
        void ClearPreview();
        SliceOptions MakeSliceOptions() const;

        // ---- state, grouped by what it belongs to ----
        // patch -> (slice) -> preview / confirmed cut. Candidate is the cut mode's choice of the patch.

        // The patch that is sliced: extracted for the cut joints in debug mode.
        // Written by UpdateCandidate / DropPatch, skinned every frame by Update.
        struct PatchState
        {
            ProceduralMesh extracted;       // patch as extracted (CPU copy)
            BonePatchSource source;
            std::wstring statusText;
            PatchPiece piece;               // welded copy of the patch: the slicer input
            WeldStats weld;
            SliceStatus status = SliceStatus::InvalidInput;     // of the welding
            std::string detail;
            float radius = 0.0f;            // bounds radius when the patch was copied (keeps debug sizes stable)
            std::vector<std::vector<DirectX::XMFLOAT4X4>> skinMatrices;     // per source mesh, at this frame's pose
            std::vector<Math::Matrix4> poseJointWorld;                      // palette joints, at this frame's pose
        };

        // Cut mode's choice: the bone segments the plane crosses and the joint set the patch was extracted for.
        // Written by UpdateCandidate every frame in cut mode.
        struct CandidateState
        {
            uint32_t palette = 0;           // chosen when T was pressed
            SkeletonPose skeleton;
            std::vector<SegmentHit> hits;
            std::vector<uint32_t> cutJoints;
            std::vector<uint32_t> attemptJoints;    // the last joint set an extraction was tried for
            bool haveAttempt = false;
            std::wstring error;
            float extractMs = 0.0f;
            uint32_t extractCount = 0;

            // Forgets what the plane crossed and the attempt cache, so the next frame extracts again.
            void ResetSelection()
            {
                hits.clear();
                cutJoints.clear();
                attemptJoints.clear();
                haveAttempt = false;
            }
        };

        // A dry-run of the plane on the uncut patch, kept only to be shown. Written by RunPreview.
        struct PreviewState
        {
            SliceResult result;
            bool have = false;
            SliceStatus status = SliceStatus::InvalidInput;
            uint64_t generation = 0;        // changes on every dry-run and every clear (the view rebakes on it)
            float ms = 0.0f;
            bool dirty = false;             // plane, cap, patch or pose changed: the dry-run is out of date
            std::vector<Math::Matrix4> previousPose;    // to notice that the animation moved

            // Forgets the dry-run; the generation changes so the view drops its loops.
            void Clear()
            {
                have = false;
                dirty = false;
                status = SliceStatus::InvalidInput;
                result = SliceResult();
                ++generation;
            }
        };

        // The pieces of the last Ok cut and the report of the last attempt. Written by ConfirmCut, skinned
        // every frame by Update with the skin slots of the patch they were cut from.
        struct ConfirmedCut
        {
            bool have = false;
            PatchPiece positive, negative;
            DirectX::XMFLOAT3 shownNormal = { 0.0f, 1.0f, 0.0f };
            uint64_t generation = 0;        // changes when the pieces are replaced or cleared (the view rebakes on it)
            BonePatchSource source;         // meshes of the patch the pieces were cut from (their skin slots)
            std::vector<std::vector<DirectX::XMFLOAT4X4>> skinMatrices;
            bool attempted = false;
            SliceStatus status = SliceStatus::InvalidInput;     // of the last attempt, Ok or not
            std::string detail;
            std::wstring info;
            float ms = 0.0f;

            // Drops the pieces and the report; the generation changes so the view drops its meshes.
            void Clear()
            {
                have = false;
                attempted = false;
                status = SliceStatus::InvalidInput;
                detail.clear();
                info.clear();
                positive.Clear();
                negative.Clear();
                ++generation;
            }
        };

        PatchState m_Patch;
        CandidateState m_Candidate;
        PreviewState m_Preview;
        ConfirmedCut m_Confirmed;

        // cut plane: a finite rectangle in world space (the space of the posed patch), tied to no joint
        bool m_PlaneEditing = false;
        bool m_CutRequested = false;    // Enter: one confirmed cut in the next Update
        bool m_PlaneValid = false;
        CutPlane m_Plane;
        float m_ModelRadius = 0.0f;     // of the instance when the plane was placed

        // animation follow-up
        uint32_t m_SkinnedNodes = 0;    // nodes skinned in the last frame (patch + pieces)
        float m_SkinMs = 0.0f;          // Skinning::Apply 호출의 CPU 시간 (지수 평균)
        std::string m_SkinError;
        bool m_FramePoseValid = false;
    };
}
