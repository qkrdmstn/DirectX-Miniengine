#include "PhysicsSystem.h"
#include "Utility.h"
#include <cstring>
#include <stdexcept>

namespace Viewer
{
    // 삭제·geometry/편집 변경 body를 제거한 뒤 CPU 충돌을 가진 메시를 독립 강체로 등록한다.
    void PhysicsSystem::SyncBodies()
    {
        // 이전 body를 다음 step 전에 정리하고 컴포넌트의 소유 표시도 해제한다.
        for (auto it = m_Bodies.begin(); it != m_Bodies.end();)
        {
            auto* object = m_Scene.FindObject(it->first.first);
            auto* mesh = object ? object->FindMeshComponent(it->first.second) : nullptr;
            const bool current = mesh && mesh->GetGeometry() == it->second.Geometry &&
                mesh->GetEditGeneration() == it->second.EditGeneration;
            if (current) { ++it; continue; }
            m_World.DestroyBody(it->second.Handle);
            if (mesh) mesh->m_Simulating = false;
            it = m_Bodies.erase(it);
        }
        if (!m_SimulationEnabled) return;

        // 삭제된 바닥 body도 다음 step 전에 정리한다.
        const auto* floor = m_Scene.FindObject(m_FloorOwner);
        if (m_FloorBody && (!floor || !floor->FindMeshComponent(m_FloorMesh)))
        {
            m_World.DestroyBody(m_FloorBody);
            m_FloorBody = MiniPhysics::InvalidBody;
            if (!floor) m_FloorOwner = InvalidObjectId;
            m_FloorMesh = InvalidComponentId;
        }

        // GPU pose 준비 여부와 무관하게 현재 충돌 hull로 등록한다.
        for (const auto& object : m_Scene.GetObjects())
            for (const auto& mesh : object->GetMeshComponents())
            {
                if (object->GetId() == m_FloorOwner) continue;
                if (mesh->GetCollision().ConvexElems.empty()) continue;
                const Key key(object->GetId(), mesh->GetId());
                const Math::Matrix4 matrix(mesh->GetWorldTransform(object->GetTransform()));
                const auto previous = m_Bodies.find(key);
                if (previous != m_Bodies.end())
                {
                    if (previous->second.Handle || (previous->second.ReservedAtFailure == m_World.GetReservedCount() &&
                        std::memcmp(&previous->second.World, &matrix, sizeof(matrix)) == 0))
                        continue;
                    m_Bodies.erase(previous);
                }

                // 실패 요청도 기록해 같은 입력을 매 프레임 재시도하거나 로그를 반복하지 않는다.
                auto& entry = m_Bodies[key];
                entry.Geometry = mesh->GetGeometry();
                entry.EditGeneration = mesh->GetEditGeneration();
                entry.World = matrix;
                entry.ReservedAtFailure = m_World.GetReservedCount();
                SceneTransform world;
                if (!SceneTransform::TryFromAffine(mesh->GetWorldTransform(object->GetTransform()), world))
                {
                    Utility::Printf("Physics body rejected: owner=%llu mesh=%llu unsupported world TRS\n", key.first, key.second);
                    continue;
                }

                // cook과 용량 예약을 먼저 완료해 실패 시 컴포넌트의 기존 배치를 보존한다.
                using namespace DirectX;
                XMFLOAT3 p, s;
                XMFLOAT4 q;
                XMStoreFloat3(&p, world.GetTranslation());
                XMStoreFloat3(&s, world.GetScale());
                XMStoreFloat4(&q, world.GetRotation());
                MiniPhysics::BodyDesc desc;
                desc.Geometry = &mesh->GetCollision();
                desc.Position = { p.x, p.y, p.z };
                desc.Scale = { s.x, s.y, s.z };
                desc.Rotation = { q.x, q.y, q.z, q.w };
                MiniPhysics::PreparedBody prepared;
                std::string error;
                if (!m_World.PrepareBody(desc, prepared, &error))
                {
                    Utility::Printf("Physics body rejected: owner=%llu mesh=%llu %s\n", key.first, key.second, error.c_str());
                    continue;
                }

                // 준비가 끝난 body와 월드 독립 배치를 같은 장면 경계에서 게시한다.
                if (!mesh->MakeWorldIndependent(object->GetTransform())) continue;
                entry.Handle = m_World.CommitBody(std::move(prepared), true);
                mesh->m_Simulating = entry.Handle != MiniPhysics::InvalidBody;
                entry.EditGeneration = mesh->GetEditGeneration();
                Utility::Printf("Physics body registered: owner=%llu mesh=%llu body=%llu\n", key.first, key.second, entry.Handle);
            }
    }

    // 하나의 공통 world에서 조회 body를 격리한 채 simulation을 진행한다.
    bool PhysicsSystem::StepSimulation(float seconds)
    {
        // 비활성 world는 SDK에 진입하지 않는다.
        if (!m_SimulationEnabled || !m_World.Step(seconds)) return false;
        ++m_StepCount;
        return true;
    }

    // 실행 상태가 실제로 바뀔 때만 시계를 비워 pause 시간을 다음 step에 섞지 않는다.
    void PhysicsSystem::SetSimulationRunning(bool running)
    {
        // body의 pose·속도·sleep은 변경하지 않는다.
        if (running == m_Running) return;
        m_Running = running;
        m_StepClock.Reset();
        Utility::Printf("Physics simulation: %s\n", running ? "running" : "paused");
    }

    // 입력 뒤 GPU 준비 전에 오래된 body를 정리하고 공통 world를 고정 간격으로 진행한다.
    unsigned PhysicsSystem::AdvanceSimulation(double deltaSeconds)
    {
        // pause 또는 zero-step 프레임에서도 삭제 body는 먼저 제거한다.
        SyncBodies();
        const unsigned steps = m_StepClock.Advance(deltaSeconds, m_SimulationEnabled && m_Running);

        // 실패한 solver step은 성공으로 보고하지 않으며 렌더용 pose는 마지막에 한 번 반영한다.
        for (unsigned i = 0; i < steps; ++i)
            if (!StepSimulation(float(PhysicsStepClock::FixedSeconds)))
                throw std::runtime_error("Physics simulation step failed");
        ApplyBodyPoses();
        return steps;
    }

    // COM을 보정하지 않고 Jolt가 반환한 body 원점 T/R을 메시의 물리 pose로 기록한다.
    void PhysicsSystem::ApplyBodyPoses()
    {
        // 살아 있는 등록 body만 읽고 scale은 컴포넌트에 남긴다.
        for (const auto& entry : m_Bodies)
        {
            if (!entry.second.Handle) continue;
            auto* object = m_Scene.FindObject(entry.first.first);
            auto* mesh = object ? object->FindMeshComponent(entry.first.second) : nullptr;
            std::array<double, 3> p;
            std::array<double, 4> q;
            if (mesh && m_World.GetBodyPose(entry.second.Handle, p, q))
                mesh->SetPhysicsPose(Math::Vector3(float(p[0]), float(p[1]), float(p[2])),
                    Math::Quaternion(DirectX::XMVectorSet(float(q[0]), float(q[1]), float(q[2]), float(q[3]))));
        }
    }

    // 실패를 기록한 항목을 제외하고 실제 동적 메시 body만 센다.
    size_t PhysicsSystem::GetSimulationBodyCount() const
    {
        // 유효 handle 하나가 메시 body 하나에 대응한다.
        size_t count = 0;
        for (const auto& entry : m_Bodies) if (entry.second.Handle) ++count;
        return count;
    }

    // 등록된 메시 body 중 깨어 있는 것만 센다.
    size_t PhysicsSystem::GetAwakeBodyCount() const
    {
        // 유효 handle 중 Jolt가 활성으로 보고하는 body를 합산한다.
        size_t count = 0;
        for (const auto& entry : m_Bodies)
            if (entry.second.Handle && m_World.IsBodyAwake(entry.second.Handle)) ++count;
        return count;
    }

    // Scene ID로 대응하는 body의 sleep 상태를 읽는다.
    bool PhysicsSystem::IsSimulationAwake(ObjectId owner, ComponentId mesh) const
    {
        // 없거나 등록에 실패한 항목은 깨어 있지 않다.
        const auto entry = m_Bodies.find(Key(owner, mesh));
        return entry != m_Bodies.end() && m_World.IsBodyAwake(entry->second.Handle);
    }
}
