#include "GeometryBuffers.h"
#include "GraphicsCore.h"
#include "CommandListManager.h"

// 결과·이전 revision·실패한 작업 자원의 마지막 참조가 사라질 때 GPU 사용을 마친다.
GeometryBuffers::~GeometryBuffers()
{
    Graphics::g_CommandManager.IdleGPU();
}
