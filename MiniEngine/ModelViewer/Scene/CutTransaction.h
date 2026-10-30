#pragma once

#include "PhysicsSystem.h"

namespace Viewer
{
    // 프레임당 새로 만드는 body 수를 제한한다. 지금은 Update 한 번에 GPU 작업 하나만 끝나므로 평소에는 닿지 않는 안전 상한이다.
    class CutBudget
    {
    public:
        // 한 프레임에 transaction이 새로 만들 수 있는 body 수(조각 2개 transaction 두 번분)다.
        static constexpr uint32_t MaxNewBodiesPerFrame = 4;
        // 현재 프레임의 남은 예산에서 bodies개를 차감한다. 초과하면 아무것도 차감하지 않고 false다.
        bool TryConsume(uint32_t bodies);
        // 이미 쓴 예산이 있고 요청 자체는 가능한 크기일 때 다음 프레임으로 미룰지 확인한다.
        bool ShouldDefer(uint32_t bodies);
        // 진단용으로 프레임당 상한을 낮춘다. 기본값은 MaxNewBodiesPerFrame이며 생산 경로는 호출하지 않는다.
        void SetLimit(uint32_t limit) { m_Limit = limit; }

    private:
        uint64_t m_Frame = ~0ull;
        uint32_t m_Used = 0;
        uint32_t m_Limit = MaxNewBodiesPerFrame;
    };

    // simulate 메시 절단의 body 교체 transaction이다: 준비 → 검증 → 교체(Commit) → 실패 시 파괴자에서 rollback.
    // 준비된 body와 B의 매핑 칸은 Commit 전에 파괴되면 모두 회수되고 부모 body와 매핑은 그대로 남는다.
    class CutTransaction
    {
    public:
        // 실행·정지·sleep 여부와 무관하게 부모 body의 존재를 검사한다.
        // 부모 body가 없으면 false와 거절 이유를 돌려준다.
        static bool CanCut(const PhysicsSystem& physics, ObjectId owner, ComponentId mesh, const char*& reason);
        // 미게시 자원과 B의 매핑 칸을 회수한다.
        ~CutTransaction();
        // 부모 body를 검증하고 A/B body와 B 매핑 칸을 준비한다.
        // 실패하면 이유를 남기고 false이며 아무것도 바꾸지 않는다.
        bool Prepare(PhysicsSystem& physics, CutBudget& budget, const GameObject& owner, const MeshComponent& parent,
            const MiniCollision::AggregateGeom& collisionA,
            const MiniCollision::AggregateGeom& collisionB, ComponentId pieceBId);
        // 부모 body를 파괴하고 준비한 body를 올리며 매핑을 A/B 새 revision으로 갱신한다. 메시 commit 직후에 호출하며 할당하지 않는다.
        void Commit(MeshComponent& a, MeshComponent& b) noexcept;
        // 마지막 Prepare 실패 이유(정적 문자열)를 읽는다.
        const char* GetFailure() const { return m_Failure; }
        // 진단용 cook 실패 주입이다: 0 꺼짐, 1 A, 2 B. 기본은 꺼져 있다.
        static int DiagFailSide;

    private:
        // 조각 하나의 body를 준비한다. 충돌이 비면 body 없는 조각으로 성공 처리한다.
        bool PrepareSide(const GameObject& owner, const MeshComponent& parent, const MiniCollision::AggregateGeom& collision,
            MiniPhysics::PreparedBody& out);
        // 준비된 body를 올리고 매핑 칸을 채운다.
        // body가 없으면 칸을 지운다.
        void CommitSide(MeshComponent& mesh, MiniPhysics::PreparedBody& prepared, PhysicsSystem::Key key) noexcept;

        PhysicsSystem* m_Physics = nullptr;
        PhysicsSystem::Key m_ParentKey{ InvalidObjectId, InvalidComponentId };
        PhysicsSystem::Key m_PieceBKey{ InvalidObjectId, InvalidComponentId };
        MiniPhysics::PreparedBody m_BodyA;
        MiniPhysics::PreparedBody m_BodyB;
        bool m_ReservedB = false;
        const char* m_Failure = nullptr;
    };
}
