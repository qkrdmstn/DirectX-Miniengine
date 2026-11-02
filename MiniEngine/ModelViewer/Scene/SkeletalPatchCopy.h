#pragma once
#include "MeshComponent.h"
#include "ReadbackBuffer.h"
#include "../../MeshCutting/Geometry/CutPlane.h"

namespace Viewer
{
    struct SkeletalPatchResult
    {
        uint32_t NodeId = UINT32_MAX;
        uint32_t CrossingTriangles = 0;
        uint32_t PatchTriangles = 0;
    };

    // 단일 제출 작업이다. 호출자는 UpdateFrame/Finish 후 component-local 평면을 전달한다.
    class SkeletalPatchJob
    {
    public:
        // 제출 자원이 GPU에서 해제될 때까지 수명을 보장한다.
        ~SkeletalPatchJob();
        // 프레임 pose 준비 후 component-local 평면으로 단일 작업을 제출한다.
        bool Submit(const MeshComponent& source, const MeshCutting::CutPlaneData& plane);
        // 기다리지 않고 선택 결과를 반환하며 SP-2의 패치 버퍼는 비워 둔다.
        bool Poll(SkeletalPatchResult& result, std::shared_ptr<GeometryBuffers>& patch);
    private:
        std::shared_ptr<const GeometryRevision> m_Input;
        ByteAddressBuffer m_Votes, m_Result;
        StructuredBuffer m_JointNodes;
        ReadbackBuffer m_Readback;
        uint64_t m_Fence = 0;
    };
}
