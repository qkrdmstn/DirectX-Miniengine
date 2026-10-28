#pragma once

#include "Scene.h"
#include "PhysicsStepClock.h"
#include "../../Physics/PhysicsWorld.h"
#include "../../MeshCutting/Geometry/CutPlane.h"
#include <map>

namespace Viewer
{
    // 절단 대상의 불변 입력을 보관한다. Jolt body 포인터는 전달하지 않는다.
    struct CutCandidate
    {
        ObjectId Owner;
        ComponentId Mesh;
        std::shared_ptr<const GeometryRevision> Geometry;
        uint64_t EditGeneration;
        uint64_t PhysicsPoseGeneration;   // 6단계에서 simulate 대상 재확인에 쓴다. 지금은 비교하지 않는다.
        Math::Matrix4 World;
    };

    // Scene의 collider 참조를 Jolt 조회 body에 연결하고 절단 후보 API를 제공한다.
    class PhysicsSystem
    {
    public:
        // 조회를 위한 장면 소유자를 연결한다.
        explicit PhysicsSystem(Scene& scene) : m_Scene(scene) {}
        // collider 참조를 Scene보다 먼저 정리한다.
        ~PhysicsSystem();
        // SDK 조회 사용 여부와 진단용 body 예산을 정한다.
        void Initialize(bool useJolt = true, uint32_t capacity = 4096, bool simulate = false);
        // GPU 준비와 무관하게 충돌이 있는 정적 메시를 등록하고 오래된 body를 step 전에 제거한다.
        void SyncBodies();
        // 공통 world를 한 번 진행한다. 호출자는 고정 간격과 프레임당 step 수를 관리한다.
        bool StepSimulation(float seconds);
        // body 원점 pose를 독립 메시 컴포넌트에 반영한다.
        void ApplyBodyPoses();
        // 초기화 실패 또는 명시적 비활성 여부를 읽는다.
        bool IsSimulationEnabled() const { return m_SimulationEnabled; }
        // 실행/일시정지 전환 시 누적 시간만 버리고 body 속도와 sleep은 유지한다.
        void SetSimulationRunning(bool running);
        // 프레임 경계에서 body 정리·최대 네 고정 step·pose 반영을 수행한다.
        unsigned AdvanceSimulation(double deltaSeconds);
        // 실행한 고정 step 수를 진단에서 읽는다.
        uint64_t GetSimulationStepCount() const { return m_StepCount; }
        // 등록된 동적 메시 body 수를 읽는다.
        size_t GetSimulationBodyCount() const;
        // 실행 중인지 읽는다. 일시정지 중에는 깨어 있는 body도 멈춘 것으로 본다.
        bool IsSimulationRunning() const { return m_Running; }
        // 등록된 메시 body 중 깨어 있는 수를 읽는다. 진단 절단이 모두 sleep할 때까지 기다리는 데 쓴다.
        size_t GetAwakeBodyCount() const;
        // 등록된 메시 body가 깨어 있는지 읽는다.
        bool IsSimulationAwake(ObjectId owner, ComponentId mesh) const;
        // 진단: 조회·simulation·준비 중 body의 용량 예약 합계를 읽는다.
        size_t GetReservedBodyCount() const { return m_World.GetReservedCount(); }
        // 진단: 등록된 메시 body의 선속도와 각속도를 읽는다.
        bool GetSimulationVelocity(ObjectId owner, ComponentId mesh, std::array<double, 3>& linear, std::array<double, 3>& angular) const;
        // 진단: body를 현재 pose에서 lift(m)만 위로 순간이동해 깨운다. lift가 0이면 깨우기만 한다.
        bool NudgeBodyForDiagnostic(ObjectId owner, ComponentId mesh, double lift);
        // 시작 배치 이후 별도 표시용 객체와 고정 Box 바닥을 함께 만든다.
        bool CreateFloor(std::shared_ptr<const Model> model);
        // 준비된 Static 메시 경계를 등록·갱신하고 삭제된 body를 정리한다.
        void SyncColliders();
        // 최신 SyncColliders 뒤 유한 평면 후보를 반환한다. 실제 split은 GPU가 결정한다.
        std::vector<CutCandidate> QueryCutCandidates(const MeshCutting::CutPlaneData& plane,
            float worldEpsilon = 1.0e-5f) const;
        // 남은 Scene 참조와 SDK world를 정리한다.
        void Clear();
        // 실제 Jolt body 수를 읽는다.
        size_t GetColliderCount() const { return m_World.GetColliderCount(); }
        // SDK 미사용·용량 초과 등으로 기존 경계 검사를 사용하는 수를 읽는다.
        size_t GetFallbackCount() const;
        // 진단 모드에서 기존 전수 검사와 결과가 같은지 대조한다.
        bool ValidateQueries = false;

    private:
        friend class CutTransaction;
        using Key = std::pair<ObjectId, ComponentId>;
        struct Entry
        {
            CutCandidate Candidate{};
            MiniPhysics::QueryBounds Bounds;
            Math::BoundingSphere Sphere{Math::kZero};
            MiniPhysics::ColliderHandle Handle = MiniPhysics::InvalidCollider;
            ComponentId Collider = InvalidComponentId;
            bool Seen = false;
        };
        struct BodyEntry
        {
            MiniPhysics::BodyHandle Handle = MiniPhysics::InvalidBody;
            std::shared_ptr<const GeometryRevision> Geometry;
            uint64_t EditGeneration = 0;
            size_t ReservedAtFailure = 0;
            Math::Matrix4 World{Math::kIdentity};
        };
        // 메시 준비 여부와 Static section 조건을 공통으로 검사한다.
        static bool IsEligible(const MeshComponent& mesh);
        // 기존 경계 구와 유한 사각형 AABB 검사를 유지한다.
        static bool Intersects(const Entry& entry, const MeshCutting::CutPlaneData& plane,
            const MiniPhysics::QueryBounds& rectangle, float epsilon);
        // ID·revision·편집·월드 배치가 현재 장면과 일치하는지 검사한다.
        bool IsCurrent(const Entry& entry) const;
        // 삭제·교체 시 참조와 SDK body를 함께 정리한다.
        void RemoveEntry(Entry& entry);
        Scene& m_Scene;
        MiniPhysics::PhysicsWorld m_World;
        std::map<Key, Entry> m_Entries;
        std::map<MiniPhysics::ColliderHandle, Key> m_Handles;
        std::vector<Key> m_FallbackKeys;
        std::map<Key, BodyEntry> m_Bodies;
        bool m_UseJolt = true;
        bool m_SimulationEnabled = false;
        ObjectId m_FloorOwner = InvalidObjectId;
        ComponentId m_FloorMesh = InvalidComponentId;
        MiniPhysics::BodyHandle m_FloorBody = MiniPhysics::InvalidBody;
        PhysicsStepClock m_StepClock;
        bool m_Running = true;
        uint64_t m_StepCount = 0;
    };
}
