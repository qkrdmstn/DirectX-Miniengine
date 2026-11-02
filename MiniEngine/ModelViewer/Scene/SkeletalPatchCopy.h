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
    // skin 경계를 직접 순회하여 가장 가까운 대상 하나의 본 선택을 기록한다.
    void LogSkeletalPatchSelection(const Scene& scene, const MeshCutting::CutPlaneData& plane);
    // CesiumMan의 위팔·허벅지·손에 현재 pose 기준 진단 평면을 배치한다.
    bool BuildSkeletalPatchDiagnosticPlane(const Scene& scene, uint32_t index, MeshCutting::CutPlaneData& plane);

    struct SkeletalPatchResult
    {
        uint32_t UncoveredCrossing = 0;
        uint32_t CrossingTriangles = 0;
        uint32_t PatchTriangles = 0;
    };

    // 단일 제출 작업이다. 호출자는 UpdateFrame/Finish 후 component-local 평면을 전달한다.
    class SkeletalPatchJob
    {
    public:
        // 제출 자원이 GPU에서 해제될 때까지 수명을 보장한다.
        ~SkeletalPatchJob();
        // SP-3 추출 패스 연결 전에는 제출을 거절한다.
        bool Submit(const MeshComponent& source, const MeshCutting::CutPlaneData& plane);
        // 기다리지 않고 선택 결과를 반환하며 SP-2의 패치 버퍼는 비워 둔다.
        bool Poll(SkeletalPatchResult& result, std::shared_ptr<GeometryBuffers>& patch);
    private:
        ReadbackBuffer m_Readback;
        uint64_t m_Fence = 0;
    };
}
