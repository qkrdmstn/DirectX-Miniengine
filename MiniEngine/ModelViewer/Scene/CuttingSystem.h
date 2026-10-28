#pragma once
#include "Scene.h"
#include "PhysicsSystem.h"
#include "CutTransaction.h"
#include "../../MeshCutting/GPU/GpuMeshCutter.h"
#include <deque>
#include <utility>
#include <vector>

namespace Viewer
{
    // 절단 결과의 종류다. Published만 Scene을 바꾼다.
    enum class CutResultStatus { Published, Measured, NoSplit, Cancelled, Failed };

    // 완료된 절단 하나의 소비용 기록이다. Other는 새 조각 B이며 Published일 때만 유효하다.
    struct CutResultEntry
    {
        ObjectId Owner = InvalidObjectId;
        ComponentId Mesh = InvalidComponentId;
        ComponentId Other = InvalidComponentId;
        uint64_t RevisionId = 0;    // Published면 A의 새 revision, 아니면 입력 revision
        CutResultStatus Status = CutResultStatus::NoSplit;
        uint32_t Error = 0;
        const char* Reason = nullptr;   // Failed일 때 정적 문자열 이유
        double GpuMs = 0;
        double ReadyMs = 0;
    };

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
        // 쌓인 완료 결과를 모두 넘겨 비운다. 소비자는 Last* 대신 이 목록을 읽는다.
        std::vector<CutResultEntry> DrainResults();
        // 소비되지 않아 버려진 가장 오래된 결과의 누적 수를 읽는다.
        uint32_t GetDroppedResults() const { return m_DroppedResults; }
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
        // 진단 로그 전용이다. 결과 소비는 DrainResults를 쓴다.
        ComponentId LastOther = InvalidComponentId;
        ObjectId LastOwner = InvalidObjectId;
        // 진단 전용 주입이다. 지정한 쪽(0 A, 1 B)의 계승 충돌을 비워 빈 쪽 게시를 강제한다. 기본은 -1(꺼짐)이다.
        int DiagEmptySide = -1;
        // 진단용으로 프레임당 새 body 상한을 낮출 수 있게 예산을 노출한다.
        CutBudget& GetBudget() { return m_Budget; }

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
        uint64_t m_EditGeneration = 0;
        CutBudget m_Budget;
        bool m_Publish = true;
        // 결과 목록에 한 칸을 확보한다. 가득 차 있으면 가장 오래된 결과를 버리며 게시 전에 호출해 이후 push가 할당하지 않게 한다.
        void ReserveResultSlot();
        static constexpr size_t MaxPendingResults = 256;
        std::vector<CutResultEntry> m_Results;
        uint32_t m_DroppedResults = 0;
        bool m_DropLogged = false;
        std::deque<CutCandidate> m_Pending;
        MeshCutting::CutPlaneData m_PendingPlane{};
        uint32_t m_PendingCapMode = 0;
        const char* m_StatusText = "Ready";
        bool m_BatchActive = false;
        uint32_t m_BatchApplied = 0;
    };
}
