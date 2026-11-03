#pragma once
#include "Scene.h"
#include "SegmentJointSelection.h"
#include "SkeletalPhysicsSystem.h"
#include "ReadbackBuffer.h"
#include "../../MeshCutting/Geometry/CutPlane.h"

namespace Viewer
{
    enum class SkeletalJointMode { Parent, Influence };
    // 모델 pose와 컴포넌트 section을 순수 선택 함수의 입력으로 바꾼다.
    SegmentJointSelection SelectSegmentJoint(const ModelInstance& pose, const MeshComponent& component,
        const SceneTransform& owner, const MeshCutting::CutPlaneData& plane);
    // 각 skin 모델의 골격에서 두 segment와 말단의 진단 평면을 배치한다.
    bool BuildSkeletalPatchDiagnosticPlane(const Scene& scene, uint32_t index, MeshCutting::CutPlaneData& plane);

    struct SkeletalPatchResult
    {
        uint32_t PatchTriangles = 0;
        uint32_t CrossingTriangles = 0;
        uint32_t UncoveredCrossing = 0;
    };

    struct SkeletalPatchBuffers
    {
        std::shared_ptr<GeometryBuffers> Geometry;
        ByteAddressBuffer TriangleSources; // section, draw, triangle; 12 bytes per triangle
        ByteAddressBuffer LocalBounds; // counts 0..8, local AABB 12..32, source world X min/max 36..40
        float SourceWorldWidth = 0;
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
        ReadbackBuffer m_OutputReadback;
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
    // 추출 요청의 배치 snapshot과 단 하나의 게시 owner를 소유한다.
    class SkeletalPatchCopy
    {
    public:
        // 새 C의 Static 후보 조회 전에 이전 작업과 owner를 공통 삭제 경로로 정리한다.
        void Reset(Scene& scene, PhysicsSystem& physics, CuttingSystem& cutting);
        // 현재 skin 대상과 본을 선택하고 GPU 추출 및 배치 snapshot을 저장한다.
        void Request(const Scene& scene, const MeshCutting::CutPlaneData& plane, bool diagnostic,
            SkeletalJointMode mode = SkeletalJointMode::Parent, uint32_t diagnosticCase = UINT32_MAX,
            PhysicsSystem* physics = nullptr);
        // 실제 C 경로로 두 모델의 break와 거절 조건을 순서대로 검증한다.
        bool RunBreakChecks(Scene& scene, PhysicsSystem& physics, CuttingSystem& cutting);
        // 복사본을 준비하고 Static batch 뒤 절단하여 최종 완료를 반환한다.
        bool Update(Scene& scene, CuttingSystem& cutting, SkeletalPatchResult& result);
        // 복사본 결과를 소비하여 Static 집계와 격리한다.
        bool OnCutResult(Scene& scene, const CuttingSystem& cutting, const CutResultEntry& result);
        // 진단의 중복 제출을 막기 위해 현재 요청 상태를 읽는다.
        bool IsPending() const
        {
            // 추출부터 절단 결과 소비까지 같은 요청으로 취급한다.
            return m_Job.IsPending() || m_CutPending || m_CutInFlight || m_Completed || m_RequestId;
        }
        // 자동 Static batch 완료 집계에서 직접 절단 작업을 구별한다.
        bool IsCutInFlight() const
        {
            // 결과를 소비하기 전까지 복사본의 단독 작업이다.
            return m_CutInFlight || !m_RetiredOwners.empty();
        }
    private:
        // 최종 컴포넌트 수와 cap 진단을 기록한다.
        void LogCutResult(const Scene& scene, const char* reason, uint32_t caps, uint32_t uncapped, uint32_t error);
        SkeletalPatchJob m_Job;
        PhysicsSystem* m_Physics = nullptr;
        BreakRequest m_Break;
        BreakStatus m_LastBreakStatus = BreakStatus::NotReady;
        IncomingJointResult m_Incoming;
        uint64_t m_RequestId = 0;
        unsigned m_DynamicBefore = 0;
        uint32_t m_BreakCheck = 0;
        bool m_BreakReady = false, m_CheckAwaiting = false;
        MeshCutting::CutPlaneData m_Plane;
        SkeletalPatchResult m_Result;
        std::vector<ObjectId> m_RetiredOwners;
        bool m_CutPending = false, m_CutInFlight = false, m_Completed = false;
        ObjectId m_CopyOwner = 0;
        SceneTransform m_World;
        std::string m_Name, m_ModelName;
        uint32_t m_SelectedNode = UINT32_MAX, m_DiagnosticCase = UINT32_MAX;
        SkeletalJointMode m_Mode = SkeletalJointMode::Parent;
        float m_Offset = 0;
        bool m_Diagnostic = false;
    };
}
