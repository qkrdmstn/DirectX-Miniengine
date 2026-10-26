#pragma once
#include "Scene.h"
#include "PhysicsSystem.h"
#include "../../MeshCutting/GPU/GpuMeshCutter.h"
#include <deque>

namespace Viewer
{
    // Scene 변경은 완료된 GPU 작업을 프레임 입력 경계에서 게시할 때만 수행한다.
    class CuttingSystem
    {
    public:
        // 절단 작업이 사용할 장면을 연결한다.
        explicit CuttingSystem(Scene& scene, PhysicsSystem& physics) : m_Scene(scene), m_Physics(physics) {}
        // 정점 준비가 끝난 Static 컴포넌트와 평면을 고정해 작업을 시작한다.
        bool Request(ObjectId owner, ComponentId mesh, const MeshCutting::CutPlaneData& plane, bool publish = true, float worldEpsilon = 1.0e-5f, uint32_t capMode = 0);
        // 현재 절단 영역의 Static 후보를 고정하고 이후 프레임에서 하나씩 제출한다.
        bool RequestAll(const MeshCutting::CutPlaneData& plane, uint32_t capMode = 0);
        // Scene.Update 뒤 준비된 pose로 다음 후보를 제출하고 변경된 후보는 건너뛴다.
        void SubmitPending();
        // 작은 상태를 비동기로 수집하고 아직 같은 입력일 때만 A/B를 함께 적용한다.
        bool Update();
        // 원본과 결과가 제출된 GPU 작업을 참조하는 동안 작업을 유지한다.
        void Clear();
        // 진행 중 작업이 있는지 확인한다.
        bool IsBusy() const { return bool(m_Job) || !m_Pending.empty(); }
        // 입력 거절과 마지막 작업 결과를 화면에 표시한다.
        const char* GetStatusText() const { return m_StatusText; }
        // 마지막 완료 작업의 GPU 및 요청→완료 시간을 읽는다.
        bool ValidateResults = false;
        uint32_t ExpectedMinimumLoops = 0;
        double LastGpuMs = 0, LastReadyMs = 0, LastAppliedMs = 0;
        uint32_t LastSegments = 0, LastCaps = 0, LastUncapped = 0, LastError = 0;
        ComponentId LastOther = InvalidComponentId;
        ObjectId LastOwner = InvalidObjectId;

    private:
        // 단일 요청과 대기열 내부 제출이 공유하는 GPU 작업 시작 경로다.
        bool Submit(ObjectId owner, ComponentId mesh, const MeshCutting::CutPlaneData& plane,
            bool publish, float worldEpsilon, uint32_t capMode);
        // 여러 후보 중 하나라도 성공했으면 마지막 비교차 후보가 성공 안내를 덮지 않는다.
        void FinishBatch();
        Scene& m_Scene;
        PhysicsSystem& m_Physics;
        std::unique_ptr<MeshCutting::CutJob> m_Job;
        ObjectId m_Owner = InvalidObjectId;
        ComponentId m_Mesh = InvalidComponentId;
        Math::Matrix4 m_World;
        uint64_t m_StateVersion = 0;
        bool m_Publish = true;
        std::deque<CutCandidate> m_Pending;
        MeshCutting::CutPlaneData m_PendingPlane{};
        uint32_t m_PendingCapMode = 0;
        const char* m_StatusText = "Ready";
        bool m_BatchActive = false;
        uint32_t m_BatchApplied = 0;
    };
}
