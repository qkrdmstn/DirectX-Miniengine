#include "GeometryBuffers.h"
#include "GraphicsCore.h"
#include "CommandListManager.h"

// 미게시 작업의 fence 완료 후 해제된 geometry에는 전역 GPU 대기를 추가하지 않는다.
GeometryBuffers::~GeometryBuffers()
{
    // 아직 남은 resource가 있을 때만 기존 안전 대기 후 wrapper를 정리한다.
    if (Data.GetResource() || Pose.GetResource() || Provenance.GetResource() || CapMaterial.GetResource())
        Graphics::g_CommandManager.IdleGPU();
}

// 작업 fence가 끝난 미게시 결과의 GPU buffer 소유권을 즉시 놓는다.
void GeometryBuffers::ReleaseGpuResourcesAfterCompletion() noexcept
{
    // geometry와 pose 출력 resource를 wrapper에서 해제한다.
    Data.Destroy();
    Pose.Destroy();

    // 진단 provenance와 cap material 자원도 함께 해제한다.
    Provenance.Destroy();
    CapMaterial.Destroy();
}
