#pragma once
#include "Scene.h"
#include "SegmentJointSelection.h"
#include "ReadbackBuffer.h"
#include "../../MeshCutting/Geometry/CutPlane.h"

namespace Viewer
{
    // 모델 pose와 컴포넌트 section을 순수 선택 함수의 입력으로 바꾼다.
    SegmentJointSelection SelectSegmentJoint(const ModelInstance& pose, const MeshComponent& component,
        const SceneTransform& owner, const MeshCutting::CutPlaneData& plane);
    class SkeletalPatchJob;
    // skin 대상과 본을 선택하고 현재 프레임의 추출 작업을 제출한다.
    void RequestSkeletalPatch(const Scene& scene, const MeshCutting::CutPlaneData& plane,
        SkeletalPatchJob& job, bool diagnostic);
    // CesiumMan의 위팔·허벅지·손에 현재 pose 기준 진단 평면을 배치한다.
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
        ByteAddressBuffer LocalBounds; // counts 0..8, ordered-float AABB min 12..20 / max 24..32
    };

    // 단일 제출 작업이다. 호출자는 UpdateFrame/Finish 후 component-local 평면을 전달한다.
    class SkeletalPatchJob
    {
    public:
        // 제출 자원이 GPU에서 해제될 때까지 수명을 보장한다.
        ~SkeletalPatchJob();
        // component-local 평면과 section별 팔레트 마스크로 GPU 추출을 제출한다.
        bool Submit(const MeshComponent& source, const MeshCutting::CutPlaneData& plane,
            const std::vector<std::vector<uint32_t>>& mask, uint32_t node = UINT32_MAX, bool diagnostic = false);
        // fence 완료 후 카운터·로컬 경계와 GPU 출력을 수집하고 진단 모드에서 CPU 기준과 비교한다.
        bool Poll(SkeletalPatchResult& result, std::shared_ptr<SkeletalPatchBuffers>& patch);
        // 이전 작업을 무효화하고 제출 자원의 GPU 사용 완료를 보장한다.
        void Cancel();
        // 자동 진단이 완료 전에 다음 지역을 제출하지 않도록 상태를 읽는다.
        bool IsPending() const { return m_Fence != 0; }
    private:
        // 같은 제출 명령에서 저장한 입력과 출처를 CPU 기준 함수와 비교한다.
        void Validate(const SkeletalPatchResult& result);
        ReadbackBuffer m_Readback;
        ReadbackBuffer m_SourceReadback, m_PoseReadback, m_OriginsReadback;
        StructuredBuffer m_Descriptors, m_Masks;
        ByteAddressBuffer m_Scan;
        std::shared_ptr<SkeletalPatchBuffers> m_Output;
        std::shared_ptr<const GeometryRevision> m_Input;
        MeshCutting::CutPlaneData m_Plane;
        std::vector<uint32_t> m_PoseOffsets;
        uint32_t m_Node = UINT32_MAX;
        bool m_Diagnostic = false;
        uint64_t m_Fence = 0;
    };
}
