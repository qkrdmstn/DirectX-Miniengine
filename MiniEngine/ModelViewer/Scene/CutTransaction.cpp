#include "CutTransaction.h"
#include "Display.h"
#include "Utility.h"
#include <cassert>
#include <utility>

namespace Viewer
{
    // 진단용 cook 실패 주입의 기본값(꺼짐)이다.
    int CutTransaction::DiagFailSide = 0;

    // 현재 프레임이 바뀌었으면 사용량을 비우고, 남은 예산이 충분할 때만 차감한다.
    bool CutBudget::TryConsume(uint32_t bodies)
    {
        // 새 프레임이면 사용량을 초기화한다.
        const uint64_t frame = Graphics::GetFrameCount();
        if (frame != m_Frame)
        {
            m_Frame = frame;
            m_Used = 0;
        }

        // 초과하면 차감하지 않고 거절한다.
        if (m_Used + bodies > m_Limit)
            return false;
        m_Used += bodies;
        return true;
    }

    // 현재 프레임에서 가능한 2-body 요청이 초과하면 다음 프레임으로 넘긴다.
    bool CutBudget::ShouldDefer(uint32_t bodies)
    {
        // 새 프레임이면 소비량을 비워 다음 요청을 즉시 허용한다.
        const uint64_t frame = Graphics::GetFrameCount();
        if (frame != m_Frame)
        {
            m_Frame = frame;
            m_Used = 0;
        }

        // limit보다 큰 요청은 원래 불가능한 진단 조건이므로 영구 대기시키지 않는다.
        return m_Used != 0 && bodies <= m_Limit && m_Used + bodies > m_Limit;
    }

    // 실행·정지·sleep 여부와 무관하게 교체할 부모 body가 있는지 확인한다.
    bool CutTransaction::CanCut(const PhysicsSystem& physics, ObjectId owner, ComponentId mesh, const char*& reason)
    {
        // body 매핑이 없는 simulate 메시는 교체할 부모 body가 없으므로 거절한다.
        const auto entry = physics.m_Bodies.find(PhysicsSystem::Key(owner, mesh));
        if (entry == physics.m_Bodies.end() || !entry->second.Handle)
        {
            reason = "Cut target has no physics body";
            return false;
        }

        // 유효한 부모 body는 현재 운동 상태와 무관하게 허용한다.
        return true;
    }

    // Commit되지 않은 transaction이면 B 매핑 칸을 지우고, 준비된 body는 PreparedBody 파괴자가 되돌린다.
    CutTransaction::~CutTransaction()
    {
        // 예약해 둔 칸이 남아 있을 때만 지운다.
        if (m_ReservedB && m_Physics)
            m_Physics->m_Bodies.erase(m_PieceBKey);
    }

    // 조각의 충돌과 부모의 현재 월드 pose·scale로 body를 준비한다. 충돌이 비면 body 없는 조각이다.
    bool CutTransaction::PrepareSide(const GameObject& owner, const MeshComponent& parent,
        const MiniCollision::AggregateGeom& collision, MiniPhysics::PreparedBody& out)
    {
        // 빈 쪽은 body 없이 성공 처리한다(우리 정책).
        if (collision.ConvexElems.empty())
            return true;

        // 부모 월드 배치를 TRS로 분해한다. 표현할 수 없으면 body를 만들 수 없다.
        SceneTransform world;
        if (!SceneTransform::TryFromAffine(parent.GetWorldTransform(owner.GetTransform()), world))
        {
            m_Failure = "unsupported world TRS";
            return false;
        }

        // 부모와 같은 pose·scale·질량 계수로 body 요청을 만든다.
        using namespace DirectX;
        XMFLOAT3 p;
        XMFLOAT3 s;
        XMFLOAT4 q;
        XMStoreFloat3(&p, world.GetTranslation());
        XMStoreFloat3(&s, world.GetScale());
        XMStoreFloat4(&q, world.GetRotation());
        MiniPhysics::BodyDesc desc;
        desc.Geometry = &collision;
        desc.Position = { p.x, p.y, p.z };
        desc.Scale = { s.x, s.y, s.z };
        desc.Rotation = { q.x, q.y, q.z, q.w };

        // cook과 용량 예약을 이 시점에 끝낸다.
        std::string error;
        if (!m_Physics->m_World.PrepareBody(desc, out, &error))
        {
            m_Failure = "body prepare failed (capacity or cook)";
            Utility::Printf("Cut physics prepare failed: %s\n", error.c_str());
            return false;
        }
        return true;
    }

    // 부모 body의 최신 상태·예산·용량을 확인하고 A/B body와 B 매핑 칸을 모두 준비한다.
    bool CutTransaction::Prepare(PhysicsSystem& physics, CutBudget& budget, const GameObject& owner, const MeshComponent& parent,
        const MiniCollision::AggregateGeom& collisionA,
        const MiniCollision::AggregateGeom& collisionB, ComponentId pieceBId)
    {
        // 부모 body가 아직 같은 geometry·편집 세대로 등록되어 있는지 확인한다.
        m_Physics = &physics;
        m_Failure = nullptr;
        m_ParentKey = PhysicsSystem::Key(owner.GetId(), parent.GetId());
        const auto entry = physics.m_Bodies.find(m_ParentKey);
        if (entry == physics.m_Bodies.end() || !entry->second.Handle || entry->second.Geometry != parent.GetGeometry() ||
            entry->second.EditGeneration != parent.GetEditGeneration())
        {
            m_Failure = "parent body changed";
            return false;
        }

        // 프레임 예산을 확인한다.
        const uint32_t newBodies = (collisionA.ConvexElems.empty() ? 0u : 1u) + (collisionB.ConvexElems.empty() ? 0u : 1u);
        if (!budget.TryConsume(newBodies))
        {
            m_Failure = "per-frame body budget exceeded";
            return false;
        }

        // A/B body를 준비하고 B의 매핑 칸을 미리 확보해 Commit이 할당하지 않게 한다.
        if (DiagFailSide == 1)
        {
            m_Failure = "forced cook failure A (diagnostic)";
            return false;
        }
        if (!PrepareSide(owner, parent, collisionA, m_BodyA))
            return false;
        if (DiagFailSide == 2)
        {
            m_Failure = "forced cook failure B (diagnostic)";
            return false;
        }
        if (!PrepareSide(owner, parent, collisionB, m_BodyB))
            return false;

        // 게시 직전 부모의 COM 속도를 각 조각 COM으로 옮겨 정지 중에도 계승한다.
        std::array<double, 3> linear;
        std::array<double, 3> angular;
        std::array<double, 3> parentCom;
        if (!physics.m_World.GetBodyVelocity(entry->second.Handle, linear, angular) ||
            !physics.m_World.GetBodyCenterOfMass(entry->second.Handle, parentCom))
        {
            m_Failure = "parent velocity or COM unavailable";
            return false;
        }
        for (auto* body : { &m_BodyA, &m_BodyB })
        {
            // 빈 충돌 조각에는 속도를 설정하지 않는다.
            if (!body->IsValid())
                continue;
            std::array<double, 3> com;
            if (!physics.m_World.GetPreparedCenterOfMass(*body, com))
            {
                m_Failure = "piece COM unavailable";
                return false;
            }

            // 강체 회전이 조각 COM에 만드는 선속도를 더한다.
            const std::array<double, 3> r = { com[0] - parentCom[0], com[1] - parentCom[1], com[2] - parentCom[2] };
            const std::array<double, 3> velocity = {
                linear[0] + angular[1] * r[2] - angular[2] * r[1],
                linear[1] + angular[2] * r[0] - angular[0] * r[2],
                linear[2] + angular[0] * r[1] - angular[1] * r[0] };
            if (!physics.m_World.SetPreparedVelocity(*body, velocity, angular))
            {
                m_Failure = "piece velocity prepare failed";
                return false;
            }
        }

        // B 매핑 칸까지 예약해 commit에서 할당하지 않는다.
        try
        {
            m_PieceBKey = PhysicsSystem::Key(owner.GetId(), pieceBId);
            physics.m_Bodies.try_emplace(m_PieceBKey);
            m_ReservedB = true;
        }
        catch (const std::exception&)
        {
            m_Failure = "mapping allocation failed";
            return false;
        }
        return true;
    }

    // 준비된 body가 있으면 올려 매핑을 현재 revision에 맞추고, 없으면 칸을 지워 body 없는 조각으로 둔다.
    void CutTransaction::CommitSide(MeshComponent& mesh, MiniPhysics::PreparedBody& prepared, PhysicsSystem::Key key) noexcept
    {
        // 준비 단계에서 계승한 속도로 시작하며 중력·접촉을 받도록 활성화한다.
        auto entry = m_Physics->m_Bodies.find(key);
        const bool hadBody = prepared.IsValid();
        const MiniPhysics::BodyHandle handle = hadBody
            ? m_Physics->m_World.CommitBody(std::move(prepared), true) : MiniPhysics::InvalidBody;

        // 유효한 준비 body의 Commit은 실패하지 않으므로 InvalidBody는 프로그래밍 오류다.
        assert(!hadBody || handle != MiniPhysics::InvalidBody);

        // body가 없으면 simulate 표시와 매핑을 제거한다.
        // 다음 SyncBodies가 충돌이 있으면 다시 등록한다.
        mesh.m_Simulating = handle != MiniPhysics::InvalidBody;
        if (!mesh.m_Simulating)
        {
            m_Physics->m_Bodies.erase(entry);
            return;
        }

        // 매핑을 새 geometry와 편집 세대로 갱신해 stale body로 오인되지 않게 한다.
        entry->second.Handle = handle;
        entry->second.Geometry = mesh.GetGeometry();
        entry->second.EditGeneration = mesh.GetEditGeneration();
        entry->second.ReservedAtFailure = 0;
        entry->second.World = Math::Matrix4(mesh.GetWorldTransform(SceneTransform(Math::kIdentity)));
    }

    // 부모 body를 먼저 파괴해 예약을 반납한 뒤 A는 부모 칸을, B는 미리 확보한 칸을 사용한다.
    void CutTransaction::Commit(MeshComponent& a, MeshComponent& b) noexcept
    {
        // 부모 body를 world에서 빼고 파괴한다.
        const MiniPhysics::BodyHandle parentHandle = m_Physics->m_Bodies.find(m_ParentKey)->second.Handle;
        m_Physics->m_World.DestroyBody(parentHandle);

        // A/B body를 올리고 매핑을 갱신한다. 이후 파괴자는 B 칸을 지우지 않는다.
        m_ReservedB = false;
        CommitSide(a, m_BodyA, m_ParentKey);
        CommitSide(b, m_BodyB, m_PieceBKey);
    }
}
