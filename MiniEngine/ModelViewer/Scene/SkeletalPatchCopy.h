#pragma once
#include "Scene.h"
#include "SegmentJointSelection.h"
#include "SkeletalPhysicsSystem.h"
#include "ReadbackBuffer.h"
#include "SkeletalBoundaryChecks.h"
#include "PatchSkinning.h"
#include "../../MeshCutting/Geometry/CutPlane.h"

namespace Viewer
{
    enum class SkeletalJointMode { Rig, Parent, Influence };
    // 모델 pose와 컴포넌트 section을 순수 선택 함수의 입력으로 바꾼다.
    SegmentJointSelection SelectSegmentJoint(const ModelInstance& pose, const MeshComponent& component,
        const SceneTransform& owner, const MeshCutting::CutPlaneData& plane);
    // 각 skin 모델의 골격에서 두 segment와 말단의 진단 평면을 배치한다.
    bool BuildSkeletalPatchDiagnosticPlane(const Scene& scene, uint32_t index, MeshCutting::CutPlaneData& plane);

    // break 진단과 시연의 기존 case 평면을 현재 pose에서 구성한다.
    MeshCutting::CutPlaneData BuildSkeletalBreakPlane(const GameObject& object, uint32_t index);

    struct SkeletalPatchResult
    {
        uint32_t PatchTriangles = 0;
        uint32_t CrossingTriangles = 0;
        uint32_t UncoveredCrossing = 0;
    };

    struct SkeletalPatchBuffers
    {
        struct BoundarySkin
        {
            uint32_t Joints[4];
            uint16_t Weights[4];
        };
        std::shared_ptr<GeometryBuffers> Geometry;
        ByteAddressBuffer TriangleSources; // section, draw, triangle; 12 bytes per triangle
        ByteAddressBuffer LocalBounds; // counts 0..8, local AABB 12..32, source world X min/max 36..40
        float SourceWorldWidth = 0;
        std::vector<uint32_t> Triangles;
        std::vector<BoundaryVertex> Boundary; // 진단에서만 채우는 C 시점 바깥 경계 정점
        std::vector<BoundarySkin> BoundarySkins;
    };

    // 단일 제출 작업이다. 호출자는 UpdateFrame/Finish 후 component-local 평면을 전달한다.
    class SkeletalPatchJob
    {
    public:
        // 제출 자원이 GPU에서 해제될 때까지 수명을 보장한다.
        ~SkeletalPatchJob();
        // component-local 평면과 section별 팔레트로 GPU 선택·추출을 제출한다.
        bool Submit(const MeshComponent& source, const MeshCutting::CutPlaneData& plane,
            const std::vector<std::vector<uint32_t>>& mask, const Math::Matrix4& world,
            uint32_t node = UINT32_MAX, bool diagnostic = false, bool influence = false);
        // fence 완료 후 카운터·로컬 경계와 GPU 출력을 수집하고 진단 모드에서 CPU 기준과 비교한다.
        bool Poll(SkeletalPatchResult& result, std::shared_ptr<SkeletalPatchBuffers>& patch);
        // 이전 작업을 무효화하고 제출 자원의 GPU 사용 완료를 보장한다.
        void Cancel();
        // 완료된 GPU 선택을 로깅에만 사용한다.
        uint32_t SelectedNode() const
        {
            // 추출과 같은 제출의 NodeId이다.
            return m_Node;
        }
        // 자동 진단이 완료 전에 다음 지역을 제출하지 않도록 상태를 읽는다.
        bool IsPending() const { return m_Fence != 0; }
    private:
        // 같은 제출 명령에서 저장한 입력과 출처를 CPU 기준 함수와 비교한다.
        void Validate(const SkeletalPatchResult& result);
        ReadbackBuffer m_Readback;
        ReadbackBuffer m_SourceReadback, m_PoseReadback, m_OriginsReadback;
        ReadbackBuffer m_OutputReadback, m_AdjacentReadback;
        StructuredBuffer m_EmptyState;
        ByteAddressBuffer m_Adjacent;
        std::vector<uint32_t> m_State;
        Math::Matrix4 m_SourceWorld;
        StructuredBuffer m_Descriptors, m_Masks;
        ByteAddressBuffer m_Scan, m_Votes, m_Selected;
        bool m_Influence = false;
        std::shared_ptr<SkeletalPatchBuffers> m_Output;
        std::shared_ptr<const GeometryRevision> m_Input;
        MeshCutting::CutPlaneData m_Plane;
        std::vector<uint32_t> m_PoseOffsets;
        uint32_t m_Node = UINT32_MAX;
        bool m_Diagnostic = false;
        uint64_t m_Fence = 0;
    };
    class PhysicsSystem;
    class CuttingSystem;
    struct CutResultEntry;
    // 현재 owner의 자동 rig를 등록하며 이미 등록되어 있으면 그대로 성공한다.
    bool RegisterAutoRig(const Scene& scene, ObjectId owner, SkeletalPhysicsSystem& skeletal);
    // 실제 Rig 선택이 Head D5인 진단 평면을 구성한다.
    bool BuildRigBoneCutPlane(const Scene& scene, PhysicsSystem& physics, MeshCutting::CutPlaneData& plane, bool joint);
    struct RigBoneCutPublication
    {
        ObjectId Owner = InvalidObjectId;
        ComponentId Component = InvalidComponentId;
        MeshCutting::CutPlaneData Plane;
        uint64_t EditGeneration = 0, Revision = 0;
        uint32_t Node = UINT32_MAX;
    };

    // 추출 요청의 배치 snapshot과 단 하나의 게시 owner를 소유한다.
    class SkeletalPatchCopy
    {
    public:
        // 새 C의 Static 후보 조회 전에 이전 작업과 owner를 공통 삭제 경로로 정리한다.
        void Reset(Scene& scene, PhysicsSystem& physics, CuttingSystem& cutting);
        // 현재 skin 대상과 본을 선택하고 GPU 추출 및 배치 snapshot을 저장한다.
        void Request(const Scene& scene, const MeshCutting::CutPlaneData& plane, bool diagnostic,
            SkeletalJointMode mode = SkeletalJointMode::Rig, uint32_t diagnosticCase = UINT32_MAX,
            PhysicsSystem* physics = nullptr);
        // 실제 C 경로로 두 모델의 break와 거절 조건을 순서대로 검증한다.
        bool RunBreakChecks(Scene& scene, PhysicsSystem& physics, CuttingSystem& cutting);
        // SP7e2 실패 집계를 진단 프로세스의 종료 판정에 전달한다.
        bool HasBoundaryCheckFailure() const
        {
            // case의 Reset 이후에도 보존한 실패 횟수를 읽는다.
            return m_BoundaryFailed != 0;
        }
        // 복사본을 준비하고 Static batch 뒤 절단하여 최종 완료를 반환한다.
        bool Update(Scene& scene, CuttingSystem& cutting, SkeletalPatchResult& result);
        // 복사본 결과를 소비하여 Static 집계와 격리한다.
        bool OnCutResult(Scene& scene, const CuttingSystem& cutting, const CutResultEntry& result);
        // 진단의 중복 제출을 막기 위해 현재 요청 상태를 읽는다.
        bool IsPending() const
        {
            // 추출부터 절단 결과 소비까지 같은 요청으로 취급한다.
            return m_Job.IsPending() || m_CutPending || m_CutInFlight || m_Completed || m_RequestId || m_BoundaryWait;
        }
        // 제거 상태를 게시하지 않는 unmask 시연의 후속 C 차단 여부를 읽는다.
        bool IsUnmaskDemoPublished() const
        {
            // 게시 이력은 Reset 이후에도 유지해 같은 실행의 재절단을 막는다.
            return m_DemoUnmask && m_DemoPublished;
        }
        // 시연 게시 직후 또는 진단 N 프레임 뒤의 모델별 캡처 번호를 한 번만 돌려준다.
        uint32_t TakeCaptureRequest()
        {
            // 요청이 없으면 0이며 읽은 요청은 즉시 지운다.
            const uint32_t request = m_CaptureRequest;
            m_CaptureRequest = 0;
            return request;
        }
        // 자동 Static batch 완료 집계에서 직접 절단 작업을 구별한다.
        bool IsCutInFlight() const
        {
            // 결과를 소비하기 전까지 복사본의 단독 작업이다.
            return m_CutInFlight || !m_RetiredOwners.empty();
        }
        // 후속 AR 단계가 준비된 부착 패치와 요청 평면을 한 번 가져간다.
        bool TakeRigBoneCutPublication(RigBoneCutPublication& publication)
        {
            // 게시 결과는 Reset 또는 다음 요청 전까지 보존한다.
            if (!m_BonePublication.Component) return false;
            publication = m_BonePublication;
            m_BonePublication = {};
            return true;
        }
    private:
        // 동기 skin VB 생성과 선택적 t_cut CPU 진단을 마친 뒤 조각에 입력을 보관한다.
        void BuildPieceSkin(MeshComponent& piece, const GeometryBuffers& geometry,
            SkeletalPatchBuffers& patch, uint32_t side);
        std::shared_ptr<SkeletalPatchBuffers> m_SkinPatch;
        // 추출된 section의 공통 N을 확정하고 체크 실행에서만 PS-1 결과를 기록한다.
        void CheckPatchSkin(const SkeletalPatchBuffers& patch);
        PatchSkinSnapshot m_SkinSnapshots[2];
        std::vector<Math::Matrix4> m_SkinSectionNodes;
        float m_SkinPaletteErrors[2]{};
        uint32_t m_SkinSnapshotCount = 0, m_SkinCheckCase = 0;
        // break 없이 추출된 패치를 원래 owner에 부착하고 원본 영역을 숨긴다.
        void PublishRigBonePatch(Scene& scene, const SkeletalPatchResult& result,
            const std::shared_ptr<SkeletalPatchBuffers>& patch);
        bool m_BonePending = false;
        ObjectId m_BoneOwner = InvalidObjectId;
        NodeAttachment m_BoneAttachment{};
        RigBoneCutPublication m_BonePublication{};
        // 원본 mask·부착 id·물리 상태를 비교 가능한 정수열로 읽는다.
        std::vector<uint64_t> ReadOriginalState(const Scene& scene, PhysicsSystem& physics) const;
        // 현재 프레임의 A/B 외곽 경계 최대 오차를 기록한다.
        void MeasureBoundaryAt(const Scene& scene, const char* label);
        // 강제 Stale 요청의 결과가 원본을 바꾸지 않았는지 확인한다.
        void VerifyStale(const Scene& scene, PhysicsSystem& physics);
        // 최종 컴포넌트 수와 cap 진단을 기록한다.
        void LogCutResult(const Scene& scene, const char* reason, uint32_t caps, uint32_t uncapped, uint32_t error);
        SkeletalPatchJob m_Job;
        PhysicsSystem* m_Physics = nullptr;
        std::shared_ptr<const GeometryRevision> m_SourceRevision;
        ComponentId m_SourceMesh = 0;
        uint32_t m_MaskRevision = 0;
        std::vector<uint32_t> m_Removal;
        NodeAttachment m_AttachA{}, m_AttachB{};
        bool m_PositiveA = true, m_OffsetCopy = false, m_DemoUnmask = false;
        uint32_t m_Demo = 0;
        bool m_DemoPublished = false;
        BreakRequest m_Break;
        BreakStatus m_LastBreakStatus = BreakStatus::NotReady;
        IncomingJointResult m_Incoming;
        uint64_t m_RequestId = 0;
        unsigned m_DynamicBefore = 0;
        uint32_t m_BreakCheck = 0;
        bool m_BreakReady = false, m_CheckAwaiting = false;
        uint32_t m_StaleStage = 0, m_BoundaryWait = 0, m_CaptureRequest = 0;
        bool m_ForceStale = false;
        // 경계 측정의 PASS·FAIL·N/A 횟수는 다음 case의 Reset 이후에도 유지한다.
        uint32_t m_BoundaryPassed = 0, m_BoundaryFailed = 0, m_BoundaryNotApplicable = 0;
        std::vector<uint64_t> m_StaleBefore;
        std::vector<BoundaryVertex> m_Boundary;
        std::vector<SkeletalPatchBuffers::BoundarySkin> m_BoundarySkins;
        std::vector<BoundaryPair> m_BoundaryPairs;
        ComponentId m_PieceIds[2]{};
        double m_SkinCpuMs = 0.0;
        double m_SkinGpuMs = 0.0;
        uint32_t m_SkinTimingFrames = 0;
        MeshCutting::CutPlaneData m_Plane;
        SkeletalPatchResult m_Result;
        std::vector<ObjectId> m_RetiredOwners;
        bool m_CutPending = false, m_CutInFlight = false, m_Completed = false;
        ObjectId m_CopyOwner = 0;
        SceneTransform m_World;
        std::string m_Name, m_ModelName;
        uint32_t m_SelectedNode = UINT32_MAX, m_DiagnosticCase = UINT32_MAX;
        SkeletalJointMode m_Mode = SkeletalJointMode::Rig;
        float m_Offset = 0;
        bool m_Diagnostic = false;
    };
}
