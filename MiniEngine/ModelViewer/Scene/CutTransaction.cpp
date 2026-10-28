#include "CutTransaction.h"
#include "Display.h"
#include "Utility.h"
#include <cassert>
#include <utility>

namespace Viewer
{
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
        if (m_Used + bodies > MaxNewBodiesPerFrame)
            return false;
        m_Used += bodies;
        return true;
    }

    // 실행 중인 world에서 깨어 있는 body는 거절하고, sleep이거나 일시정지면 허용한다(R10).
    bool CutTransaction::CanCut(const PhysicsSystem& physics, ObjectId owner, ComponentId mesh, const char*& reason)
    {
        // body 매핑이 없는 simulate 메시는 교체할 부모 body가 없으므로 거절한다.
        const auto entry = physics.m_Bodies.find(PhysicsSystem::Key(owner, mesh));
        if (entry == physics.m_Bodies.end() || !entry->second.Handle)
        {
            reason = "Cut target has no physics body";
            return false;
        }

        // 일시정지 중에는 깨어 있어도 멈춘 것으로 본다.
        if (physics.m_Running && physics.m_World.IsBodyAwake(entry->second.Handle))
        {
            reason = "Wait - object moving";
            return false;
        }
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
        uint64_t jobPoseGeneration, const MiniCollision::AggregateGeom& collisionA,
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

        // GPU 작업 동안 움직였거나 깨어났으면 부모를 유지한다.
        const char* reason = nullptr;
        if (parent.GetPhysicsPoseGeneration() != jobPoseGeneration)
        {
            m_Failure = "parent pose changed during cut";
            return false;
        }
        if (!CanCut(physics, owner.GetId(), parent.GetId(), reason))
        {
            m_Failure = reason;
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
        if (!PrepareSide(owner, parent, collisionA, m_BodyA) || !PrepareSide(owner, parent, collisionB, m_BodyB))
            return false;
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
        // 조각은 속도 0으로 시작하며 중력·접촉으로 정착하도록 활성화한다.
        auto entry = m_Physics->m_Bodies.find(key);
        const bool hadBody = prepared.IsValid();
        const MiniPhysics::BodyHandle handle = hadBody
            ? m_Physics->m_World.CommitBody(std::move(prepared), true) : MiniPhysics::InvalidBody;

        // 유효한 준비 body의 Commit은 실패하지 않으므로 InvalidBody는 프로그래밍 오류다.
        assert(!hadBody || handle != MiniPhysics::InvalidBody);

        // body가 없으면 simulate 표시와 매핑을 제거한다. 다음 SyncBodies가 충돌이 있으면 다시 등록한다.
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
