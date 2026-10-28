#include "PhysicsSystem.h"
#include "Utility.h"
#include <cmath>
#include <cstring>
#include <stdexcept>

namespace Viewer
{
    // 물리 갱신을 먼저 완료하고 같은 body를 유지한 채 메시 편집을 게시한다.
    bool PhysicsSystem::ApplyMeshEdit(Scene& scene, ObjectId owner, ComponentId meshId, const SceneTransform& world)
    {
        // 다른 장면이나 잠긴 객체, 종속 메시, 잘못된 TRS는 변경 전에 거절한다.
        auto* object = scene.FindObject(owner);
        auto* mesh = object ? object->FindMeshComponent(meshId) : nullptr;
        SceneTransform normalized = world;
        if (&scene != &m_Scene || !object || !object->IsEditable() || !mesh ||
            mesh->GetTransformMode() != MeshTransformMode::WorldIndependent || !normalized.Normalize()) return false;

        // scale의 실제 세 성분을 비트 단위로 비교한다.
        DirectX::XMFLOAT3 oldScale, scale, position;
        DirectX::XMFLOAT4 rotation;
        DirectX::XMStoreFloat3(&oldScale, mesh->GetTransform().GetScale());
        DirectX::XMStoreFloat3(&scale, normalized.GetScale());
        DirectX::XMStoreFloat3(&position, normalized.GetTranslation());
        DirectX::XMStoreFloat4(&rotation, normalized.GetRotation());
        auto entry = m_Bodies.find(Key(owner, meshId));
        const bool hasBody = entry != m_Bodies.end() && entry->second.Handle;
        const bool reshape = hasBody && std::memcmp(&oldScale, &scale, sizeof(scale)) != 0;

        // 정규화된 TRS의 위치 한계를 shape 변경 전에 검사해 실패 시 sleep 상태도 보존한다.
        if (hasBody)
            for (const double p : { double(position.x), double(position.y), double(position.z) })
                if (std::abs(p) > MiniPhysics::PhysicsWorld::MaxBodyPosition) return false;

        // cook 실패는 렌더와 세대를 건드리지 않고 반환한다.
        if (reshape && !m_World.SetBodyShape(entry->second.Handle, mesh->GetCollision(),
            { scale.x, scale.y, scale.z }, MiniPhysics::BodyDesc().MassScale)) return false;

        // 순간이동 실패 시 이전 scale의 shape와 질량으로 복구한다.
        if (hasBody && !m_World.SetBodyTransform(entry->second.Handle,
            { position.x, position.y, position.z }, { rotation.x, rotation.y, rotation.z, rotation.w }))
        {
            if (reshape && !m_World.SetBodyShape(entry->second.Handle, mesh->GetCollision(),
                { oldScale.x, oldScale.y, oldScale.z }, MiniPhysics::BodyDesc().MassScale))
                throw std::runtime_error("Mesh edit shape rollback failed");
            return false;
        }

        // 검증된 TRS를 게시하고 SyncBodies가 기존 body를 유지하도록 세대를 맞춘다.
        if (!mesh->SetTransform(normalized, MeshTransformMode::WorldIndependent)) return false;
        if (hasBody) entry->second.EditGeneration = mesh->GetEditGeneration();
        return true;
    }

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

    // Scene ID로 대응하는 body의 월드 질량 중심을 읽는다.
    bool PhysicsSystem::GetSimulationCenterOfMass(ObjectId owner, ComponentId mesh, std::array<double, 3>& com) const
    {
        // 유효 handle이 있을 때만 world에서 읽는다.
        const auto entry = m_Bodies.find(Key(owner, mesh));
        return entry != m_Bodies.end() && entry->second.Handle && m_World.GetBodyCenterOfMass(entry->second.Handle, com);
    }

    // Scene ID로 대응하는 body의 속도를 읽는다. 등록되지 않았으면 false다.
    bool PhysicsSystem::GetSimulationVelocity(ObjectId owner, ComponentId mesh, std::array<double, 3>& linear, std::array<double, 3>& angular) const
    {
        // 유효 handle이 있을 때만 world에서 읽는다.
        const auto entry = m_Bodies.find(Key(owner, mesh));
        return entry != m_Bodies.end() && entry->second.Handle && m_World.GetBodyVelocity(entry->second.Handle, linear, angular);
    }

    // 정지 중인 진단 부모를 기존 준비 API로 복제해 지정 속도를 부여한다.
    bool PhysicsSystem::SeedVelocityForDiagnostic(ObjectId owner, ComponentId meshId,
        const std::array<double, 3>& linear, const std::array<double, 3>& angular)
    {
        // 진단 시나리오가 허용하지 않았으면 body를 변경하지 않는다.
        if (!DiagAllowBodyMutation)
            return false;

        // 정지된 유효 부모만 대상으로 삼고 기존 body를 보존한 채 준비한다.
        const auto entry = m_Bodies.find(Key(owner, meshId));
        const auto* object = m_Scene.FindObject(owner);
        const auto* mesh = object ? object->FindMeshComponent(meshId) : nullptr;
        if (IsSimulationRunning() || entry == m_Bodies.end() || !entry->second.Handle || !mesh)
            return false;
        SceneTransform world;
        if (!SceneTransform::TryFromAffine(mesh->GetWorldTransform(object->GetTransform()), world))
            return false;

        // 원래 충돌과 월드 배치를 사용하고 미게시 body에만 속도를 쓴다.
        DirectX::XMFLOAT3 p;
        DirectX::XMFLOAT3 scale;
        DirectX::XMFLOAT4 q;
        DirectX::XMStoreFloat3(&p, world.GetTranslation());
        DirectX::XMStoreFloat3(&scale, world.GetScale());
        DirectX::XMStoreFloat4(&q, world.GetRotation());
        MiniPhysics::BodyDesc desc;
        desc.Geometry = &mesh->GetCollision();
        desc.Position = { p.x, p.y, p.z };
        desc.Scale = { scale.x, scale.y, scale.z };
        desc.Rotation = { q.x, q.y, q.z, q.w };
        MiniPhysics::PreparedBody prepared;
        if (!m_World.PrepareBody(desc, prepared) || !m_World.SetPreparedVelocity(prepared, linear, angular))
            return false;

        // 준비가 성공한 뒤 부모 매핑의 body만 교체한다.
        m_World.DestroyBody(entry->second.Handle);
        entry->second.Handle = m_World.CommitBody(std::move(prepared), true);
        return true;
    }

    // 진단용으로 현재 pose에서 위로 lift만큼 옮겨 body를 깨우고, 옮겼으면 컴포넌트 pose에도 바로 반영한다.
    bool PhysicsSystem::NudgeBodyForDiagnostic(ObjectId owner, ComponentId mesh, double lift)
    {
        // 진단 시나리오가 허용하지 않았으면 body를 변경하지 않는다.
        if (!DiagAllowBodyMutation)
            return false;

        // 등록된 body의 현재 pose를 읽는다.
        const auto entry = m_Bodies.find(Key(owner, mesh));
        std::array<double, 3> position;
        std::array<double, 4> rotation;
        if (entry == m_Bodies.end() || !entry->second.Handle || !m_World.GetBodyPose(entry->second.Handle, position, rotation))
            return false;

        // 같은 pose 또는 lift만큼 올린 pose로 순간이동하면 body가 깨어난다.
        position[1] += lift;
        if (!m_World.SetBodyTransform(entry->second.Handle, position, rotation))
            return false;

        // 올렸다면 pose 세대가 바로 바뀌도록 컴포넌트에 반영한다.
        if (lift != 0.0)
            ApplyBodyPoses();
        return true;
    }

    // Scene ID로 대응하는 body의 sleep 상태를 읽는다.
    bool PhysicsSystem::IsSimulationAwake(ObjectId owner, ComponentId mesh) const
    {
        // 없거나 등록에 실패한 항목은 깨어 있지 않다.
        const auto entry = m_Bodies.find(Key(owner, mesh));
        return entry != m_Bodies.end() && m_World.IsBodyAwake(entry->second.Handle);
    }

    // 등록된 메시 body의 상태를 디버그 표시 색 구분용으로 읽는다.
    PhysicsSystem::BodyState PhysicsSystem::GetBodyState(ObjectId owner, ComponentId mesh) const
    {
        // body가 없거나 handle이 비면 None이다.
        const auto entry = m_Bodies.find(Key(owner, mesh));
        if (entry == m_Bodies.end() || !entry->second.Handle)
            return BodyState::None;

        // 유효한 body는 Jolt의 활성 여부로 Awake/Asleep을 나눈다.
        return m_World.IsBodyAwake(entry->second.Handle) ? BodyState::Awake : BodyState::Asleep;
    }

    // 바닥 렌더 메시인지 소유 ID로 비교한다.
    bool PhysicsSystem::IsFloorMesh(ObjectId owner, ComponentId mesh) const
    {
        // 바닥 body가 살아 있고 두 ID가 일치할 때만 참이다.
        return m_FloorBody && owner == m_FloorOwner && mesh == m_FloorMesh;
    }

    // CreateFloor가 만든 Static Box의 중심과 반크기를 돌려준다.
    bool PhysicsSystem::GetFloorBox(std::array<double, 3>& center, std::array<double, 3>& half) const
    {
        // 바닥 body가 없으면 읽을 값이 없다.
        if (!m_FloorBody)
            return false;

        // 저장해 둔 Static Box 값을 복사한다.
        center = m_FloorCenter;
        half = m_FloorHalf;
        return true;
    }
}
