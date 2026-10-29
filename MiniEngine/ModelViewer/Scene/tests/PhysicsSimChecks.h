#pragma once

#include "../PhysicsSystem.h"
#include "../CuttingSystem.h"
#include "Utility.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Viewer
{
    // 실행한 물리 진단 조건의 누적 개수를 보관한다.
    inline size_t& PhysicsSimCheckCount()
    {
        // 각 실행 프로세스에서 하나의 계수기를 공유한다.
        static size_t count = 0;
        return count;
    }

    // Release에서도 물리 장면 진단 실패를 예외로 보고한다.
    inline void CheckPhysicsSim(bool condition, const char* message)
    {
        // false 조건을 호출 위치와 무관하게 런타임 실패로 전달한다.
        ++PhysicsSimCheckCount();
        if (!condition)
        {
            Utility::Printf("Physics check failed: %s\n", message);
            Utility::Printf("Physics scenario checks at failure: %zu\n", PhysicsSimCheckCount());
            throw std::runtime_error(message);
        }
    }

    // 컴포넌트의 CPU 충돌 정점을 월드로 변환해 가장 낮은 높이를 구한다.
    inline float LowestCollisionPoint(const GameObject& object, const MeshComponent& mesh)
    {
        // 렌더와 같은 월드 행렬을 사용하며 GPU 정점을 읽지 않는다.
        const Math::AffineTransform world = mesh.GetWorldTransform(object.GetTransform());
        float lowest = std::numeric_limits<float>::max();
        for (const auto& elem : mesh.GetCollision().ConvexElems)
            for (const auto& vertex : elem.VertexData)
            {
                const Math::Vector3 point = world * Math::Vector3(float(vertex.x), float(vertex.y), float(vertex.z));
                const float height = point.GetY();
                CheckPhysicsSim(std::isfinite(height), "Physics collision point is finite");
                lowest = std::min(lowest, height);
            }
        return lowest;
    }

    // 바닥 렌더 원본 bounds의 월드 최대 높이로 실제 Box 윗면을 구한다.
    inline float PhysicsFloorTop(const GameObject& floor)
    {
        // 바닥 메시의 원본 bounds 여덟 모서리를 월드로 변환해 최대 높이를 찾는다.
        const auto& floorMesh = *floor.GetMeshComponents().front();
        const auto& bounds = floorMesh.GetGeometry()->GetSource()->m_BoundingBox;
        const Math::AffineTransform floorWorld = floorMesh.GetWorldTransform(floor.GetTransform());
        const Math::Vector3 minimum = bounds.GetMin();
        const Math::Vector3 maximum = bounds.GetMax();
        float floorTop = -std::numeric_limits<float>::max();
        for (unsigned corner = 0; corner < 8; ++corner)
        {
            const Math::Vector3 point((corner & 1) ? maximum.GetX() : minimum.GetX(),
                (corner & 2) ? maximum.GetY() : minimum.GetY(), (corner & 4) ? maximum.GetZ() : minimum.GetZ());
            floorTop = std::max(floorTop, float((floorWorld * point).GetY()));
        }
        CheckPhysicsSim(std::isfinite(floorTop), "Floor top is finite");
        return floorTop;
    }

    // 절단 뒤 body 수 보존, 빈 조각의 body 없음, 모든 조각의 sleep과 바닥 위 정착을 검사한다.
    inline void RunPhysicsCutChecks(Scene& scene, PhysicsSystem& physics, size_t baselineBodies, uint32_t publications)
    {
        // 바닥 윗면과 절단 가능 메시의 body 보유 수를 센다.
        const GameObject* floor = nullptr;
        for (const auto& object : scene.GetObjects())
            if (object->GetName() == "Physics floor") floor = object.get();
        CheckPhysicsSim(floor, "Physics floor exists after cuts");
        const float floorTop = PhysicsFloorTop(*floor);
        size_t withBody = 0;
        size_t bodyless = 0;
        float lowest = std::numeric_limits<float>::max();

        // 절단 가능한 메시마다 body 보유 여부가 convex 유무와 일치하는지 확인한다.
        for (const auto& object : scene.GetObjects())
            for (const auto& mesh : object->GetMeshComponents())
            {
                if (!object->IsCuttable()) continue;
                const bool hasConvex = !mesh->GetCollision().ConvexElems.empty();
                CheckPhysicsSim(mesh->IsSimulating() == hasConvex, "A piece has a body exactly when it has convex collision");
                if (!hasConvex)
                {
                    ++bodyless;
                    continue;
                }

                // 조각이 깨어 있지 않고 바닥 관통 한계 안에 있는지 확인한다.
                ++withBody;
                const float meshLowest = LowestCollisionPoint(*object, *mesh);
                CheckPhysicsSim(!physics.IsSimulationAwake(object->GetId(), mesh->GetId()), "Cut pieces are asleep");
                CheckPhysicsSim(meshLowest >= floorTop - 0.01f, "Cut piece stays above floor penetration limit");
                lowest = std::min(lowest, meshLowest);
                Utility::Printf("Cut piece settled: owner=%llu mesh=%llu convex=%u lowest=%.6f floor=%.6f\n",
                    object->GetId(), mesh->GetId(), (uint32_t)mesh->GetCollision().ConvexElems.size(), meshLowest, floorTop);
            }

        // 메시마다 body가 하나이고, 모든 메시 수는 시작 수에 게시 수를 더한 값이다.
        CheckPhysicsSim(withBody == physics.GetSimulationBodyCount(), "Every piece with convex collision owns one body");
        CheckPhysicsSim(withBody + bodyless == baselineBodies + publications, "Mesh count equals baseline plus publications");
        Utility::Printf("Physics cut validation passed: bodies=%zu bodyless=%zu baseline=%zu publications=%u lowest=%.6f floor=%.6f\n",
            withBody, bodyless, baselineBodies, publications, lowest, floorTop);
    }

    // 첫 GPU 준비 전에 등록·낙하·sleep·pose 세대·삭제 수명을 실제 Scene과 공통 world로 검사한다.
    inline void RunPhysicsSimChecks(Scene& scene, PhysicsSystem& physics)
    {
        // simulation을 만들지 않은 실행은 메시 body와 소유 표시도 없어야 한다.
        physics.SyncBodies();
        if (!physics.IsSimulationEnabled())
        {
            CheckPhysicsSim(physics.GetSimulationBodyCount() == 0, "Disabled simulation has no mesh bodies");
            for (const auto& object : scene.GetObjects())
                for (const auto& mesh : object->GetMeshComponents())
                    CheckPhysicsSim(!mesh->IsSimulating(), "Disabled simulation has no simulated component");
            Utility::Printf("Physics simulation validation passed: disabled bodies=0\n");
            return;
        }

        // 바닥 렌더 원본 bounds의 월드 최대 높이로 실제 Box 윗면을 찾는다.
        const GameObject* floor = nullptr;
        for (const auto& object : scene.GetObjects())
            if (object->GetName() == "Physics floor") floor = object.get();
        CheckPhysicsSim(floor && !floor->GetMeshComponents().empty(), "Physics floor exists before first step");
        CheckPhysicsSim(!floor->IsCuttable() && !floor->IsEditable(), "Physics floor cannot be cut or edited");
        const float floorTop = PhysicsFloorTop(*floor);

        // 원본 메시마다 편집 세대와 최초 높이를 저장하고 GPU 준비와 무관한 등록을 확인한다.
        struct Snapshot
        {
            ObjectId Owner;
            ComponentId Mesh;
            uint64_t Edit, Pose;
            float Height;
        };
        std::vector<Snapshot> snapshots;
        size_t eligible = 0;
        std::shared_ptr<const Model> probeModel;
        for (const auto& object : scene.GetObjects())
            for (const auto& mesh : object->GetMeshComponents())
            {
                if (object->IsCuttable() && !mesh->GetCollision().ConvexElems.empty()) ++eligible;
                if (!mesh->IsSimulating()) continue;
                CheckPhysicsSim(mesh->GetTransformMode() == MeshTransformMode::WorldIndependent, "Physics mesh is world independent");
                // owner를 옮겨도 월드 독립 조각의 월드 위치는 변하지 않아야 한다.
                CheckPhysicsSim(object->IsEditable(), "Simulating owner stays editable");
                const SceneTransform ownerBefore = object->GetTransform();
                const Math::Vector3 worldBefore = mesh->GetWorldTransform(ownerBefore).GetTranslation();
                SceneTransform ownerMoved = ownerBefore;
                ownerMoved.SetTranslation(ownerBefore.GetTranslation() + Math::Vector3(1.0f, 2.0f, 3.0f));
                CheckPhysicsSim(object->SetTransform(ownerMoved), "Owner edit is accepted");
                CheckPhysicsSim(float(Math::Length(mesh->GetWorldTransform(object->GetTransform()).GetTranslation() - worldBefore)) < 1e-6f,
                    "Owner edit does not move world independent pieces");
                CheckPhysicsSim(object->SetTransform(ownerBefore), "Owner edit is restored");
                snapshots.push_back({ object->GetId(), mesh->GetId(), mesh->GetEditGeneration(),
                    mesh->GetPhysicsPoseGeneration(), float(mesh->GetTransform().GetTranslation().GetY()) });
                if (!probeModel) probeModel = mesh->GetGeometry()->GetSource();
            }
        CheckPhysicsSim(!snapshots.empty() && snapshots.size() == physics.GetSimulationBodyCount(), "All registered bodies have scene components");
        CheckPhysicsSim(snapshots.size() == eligible, "Every eligible fixture mesh registers a body");

        // 움직이는 body도 pause 동안 step과 pose 세대가 멈추고 resume 후 한 step부터 재개한다.
        CheckPhysicsSim(physics.AdvanceSimulation(PhysicsStepClock::FixedSeconds) == 1, "Frame runs one fixed step");
        const auto& first = snapshots.front();
        const auto* firstMesh = scene.FindObject(first.Owner)->FindMeshComponent(first.Mesh);
        const uint64_t pausedPose = firstMesh->GetPhysicsPoseGeneration();
        const uint64_t pausedStep = physics.GetSimulationStepCount();
        physics.SetSimulationRunning(false);
        CheckPhysicsSim(physics.AdvanceSimulation(10.0) == 0 && physics.GetSimulationStepCount() == pausedStep,
            "Pause neither steps nor accumulates time");
        CheckPhysicsSim(firstMesh->GetPhysicsPoseGeneration() == pausedPose, "Pause preserves physics pose");
        physics.SetSimulationRunning(true);
        CheckPhysicsSim(physics.AdvanceSimulation(PhysicsStepClock::FixedSeconds) == 1, "Resume has no catch-up burst");
        CheckPhysicsSim(physics.AdvanceSimulation(1.0) == 4, "Frame caps catch-up at four steps");
        CheckPhysicsSim(physics.AdvanceSimulation(0.0) == 0, "Zero-delta frame has no deferred backlog");

        // 최대 10초의 고정 step으로 낙하와 접촉을 실제 solver에서 진행한다.
        unsigned ticks = 0;
        bool allSleeping = false;
        for (; ticks < 600; ++ticks)
        {
            CheckPhysicsSim(physics.StepSimulation(1.0f / 60.0f), "Physics fixed step succeeds");
            physics.ApplyBodyPoses();
            allSleeping = true;
            for (const auto& snapshot : snapshots)
                allSleeping &= !physics.IsSimulationAwake(snapshot.Owner, snapshot.Mesh);
            if (allSleeping && ticks >= 359)
            {
                ++ticks;
                break;
            }
        }
        CheckPhysicsSim(allSleeping, "All physics fixture bodies settle within ten seconds");

        // pose만 변했고 편집 세대는 유지되며 충돌 최저점이 바닥을 과도하게 관통하지 않는지 확인한다.
        bool fell = false;
        float lowest = std::numeric_limits<float>::max();
        for (const auto& snapshot : snapshots)
        {
            const auto* object = scene.FindObject(snapshot.Owner);
            const auto* mesh = object ? object->FindMeshComponent(snapshot.Mesh) : nullptr;
            CheckPhysicsSim(mesh && mesh->IsSimulating(), "Physics fixture survives simulation");
            const Math::Vector3 position = mesh->GetTransform().GetTranslation();
            CheckPhysicsSim(std::isfinite(float(position.GetX())) && std::isfinite(float(position.GetY())) &&
                std::isfinite(float(position.GetZ())), "Physics pose is finite");
            CheckPhysicsSim(mesh->GetEditGeneration() == snapshot.Edit, "Physics does not change edit generation");
            CheckPhysicsSim(mesh->GetPhysicsPoseGeneration() > snapshot.Pose, "Physics pose generation advances");
            fell |= float(position.GetY()) < snapshot.Height - 0.05f;
            const float meshLowest = LowestCollisionPoint(*object, *mesh);
            Utility::Printf("Physics settled: owner=%llu mesh=%llu lowest=%.6f floor=%.6f awake=%u\n",
                snapshot.Owner, snapshot.Mesh, meshLowest, floorTop,
                physics.IsSimulationAwake(snapshot.Owner, snapshot.Mesh) ? 1u : 0u);
            CheckPhysicsSim(meshLowest >= floorTop - 0.01f, "Physics fixture stays above floor penetration limit");
            lowest = std::min(lowest, meshLowest);
        }
        CheckPhysicsSim(fell, "At least one physics fixture falls before sleeping");

        // 새 owner를 GPU 준비 없이 등록하고 삭제 직후 다음 step 전에 body 매핑을 제거한다.
        const size_t baseline = physics.GetSimulationBodyCount();
        auto& probe = scene.CreateObject("Physics lifetime probe", probeModel);
        const ObjectId probeId = probe.GetId();
        SceneTransform placement;
        placement.SetTranslation(Math::Vector3(30, 20, 0));
        CheckPhysicsSim(probe.SetTransform(placement), "Probe placement is valid");
        physics.SyncBodies();
        CheckPhysicsSim(physics.GetSimulationBodyCount() == baseline + 1, "Probe body registers before GPU preparation");
        CheckPhysicsSim(scene.DeleteObject(probeId), "Delete physics lifetime probe");
        physics.SyncBodies();
        CheckPhysicsSim(physics.GetSimulationBodyCount() == baseline, "Deleted probe body is removed before next step");

        // 용량 부족으로 실패한 메시도 다른 body 삭제로 빈자리가 생기면 다시 등록된다.
        Scene capacityScene;
        PhysicsSystem capacityPhysics(capacityScene);
        capacityPhysics.Initialize(false, 1, true);
        auto& firstProbe = capacityScene.CreateObject("Capacity first", probeModel);
        const ObjectId firstId = firstProbe.GetId();
        auto& waitingProbe = capacityScene.CreateObject("Capacity waiting", probeModel);
        capacityPhysics.SyncBodies();
        CheckPhysicsSim(capacityPhysics.GetSimulationBodyCount() == 1 &&
            !waitingProbe.GetMeshComponents().front()->IsSimulating(), "Second body waits for capacity");
        capacityScene.DeleteObject(firstId);
        capacityPhysics.SyncBodies();
        CheckPhysicsSim(capacityPhysics.GetSimulationBodyCount() == 1 &&
            waitingProbe.GetMeshComponents().front()->IsSimulating(), "Freed capacity retries failed registration");
        capacityPhysics.Clear();
        CheckPhysicsSim(!waitingProbe.GetMeshComponents().front()->IsSimulating(), "Clear releases simulation ownership");
        Utility::Printf("Physics simulation validation passed: bodies=%zu ticks=%u lowest=%.6f floor=%.6f sleeping=1 lifetime=1\n",
            baseline, ticks, lowest, floorTop);
    }

    // 절단 실패·예외 경로를 실제 장면에서 구동하고 검사하는 진단 시나리오다. 이름을 주지 않으면 모든 호출이 아무 일도 하지 않는다.
    class PhysicsCutScenario
    {
        enum class Kind { None, Midair, MidairSettle, DropNoCut, SpinPaused, Awake, Woken, Moved, FailA, FailB, Budget, EmptyA, EmptyB, Paused, PausedAsleep, Plain };

        // 절단 직전에 기록하는 부모와 world의 상태다.
        struct Snapshot
        {
            std::shared_ptr<const GeometryRevision> Geometry;
            uint64_t Edit = 0;
            uint64_t Pose = 0;
            size_t Meshes = 0;
            size_t Bodies = 0;
            size_t Reserved = 0;
            bool Awake = false;
            Math::AffineTransform World;
        };

    public:
        // 이름으로 시나리오를 고르고 필요한 주입 스위치를 켠다.
        // 모르는 이름은 오류다.
        void Configure(const std::wstring& name, CuttingSystem& cutting)
        {
            // 이름을 종류로 바꾼다.
            const struct
            {
                const wchar_t* Name;
                Kind Value;
            } table[] = {
                { L"midair-settle", Kind::MidairSettle }, { L"drop-no-cut", Kind::DropNoCut },
                { L"spin-paused", Kind::SpinPaused },
                { L"midair", Kind::Midair }, { L"awake", Kind::Awake }, { L"woken", Kind::Woken }, { L"moved", Kind::Moved },
                { L"fail-a", Kind::FailA }, { L"fail-b", Kind::FailB }, { L"budget", Kind::Budget },
                { L"empty-a", Kind::EmptyA }, { L"empty-b", Kind::EmptyB }, { L"paused", Kind::Paused },
                { L"paused-asleep", Kind::PausedAsleep }, { L"plain", Kind::Plain } };
            for (const auto& row : table)
            {
                if (name == row.Name)
                    m_Kind = row.Value;
            }
            if (m_Kind == Kind::None)
                throw std::invalid_argument("Unknown physics-cut-scenario");

            // 종료 때 주입을 되돌릴 수 있게 절단 시스템을 기억한다.
            m_Cutting = &cutting;

            // 종류에 맞는 실패 주입을 켠다.
            CutTransaction::DiagFailSide = m_Kind == Kind::FailA ? 1 : m_Kind == Kind::FailB ? 2 : 0;
            cutting.DiagEmptySide = m_Kind == Kind::EmptyA ? 0 : m_Kind == Kind::EmptyB ? 1 : -1;
            if (m_Kind == Kind::Budget)
                cutting.GetBudget().SetLimit(1);
        }

        // 시나리오가 켜져 있는지 읽는다.
        bool Active() const { return m_Kind != Kind::None; }

        // 공중 절단은 낙하 속도를 확인하고 정지 시나리오는 정지를 기다린다.
        bool AllowSubmit(const PhysicsSystem& physics) const
        {
            // 무절단 비교는 제출을 막고 낙하 시나리오는 속도를 기다린다.
            if (m_Kind == Kind::DropNoCut)
                return false;
            if (m_Kind == Kind::Midair || m_Kind == Kind::MidairSettle)
            {
                std::array<double, 3> linear;
                std::array<double, 3> angular;
                return m_Lifted && physics.GetSimulationVelocity(m_Owner, m_Mesh, linear, angular) && linear[1] < -0.2;
            }
            if (m_Kind == Kind::Awake)
                return true;
            if (m_Kind == Kind::Paused || m_Kind == Kind::PausedAsleep || m_Kind == Kind::SpinPaused)
                return m_PauseIssued;
            return physics.GetAwakeBodyCount() == 0;
        }

        // 낙하 중 또는 sleep 뒤에 정지를 요청하고 게시 뒤 재개한다.
        // 실행 상태 변경이 없으면 -1을 돌려준다.
        int WantedRunState(const PhysicsSystem& physics, ObjectId owner, ComponentId mesh)
        {
            // 게시와 검사가 끝나면 재개를 요청한다.
            if (m_WantResume)
            {
                m_WantResume = false;
                return 1;
            }

            // 아직 정지하지 않았다면 조건에 맞는 첫 프레임에 정지한다.
            if (m_PauseIssued || (m_Kind != Kind::Paused && m_Kind != Kind::PausedAsleep && m_Kind != Kind::SpinPaused))
                return -1;
            std::array<double, 3> linear;
            std::array<double, 3> angular;
            const bool ready = m_Kind == Kind::Paused || m_Kind == Kind::SpinPaused
                ? physics.GetSimulationVelocity(owner, mesh, linear, angular) && linear[1] < -0.2
                : physics.GetSimulationBodyCount() > 0 && physics.GetAwakeBodyCount() == 0;
            if (!ready)
                return -1;
            m_PauseIssued = true;
            return 0;
        }

        // 다음 프레임 게시 직전까지 유지되는 부모 pose와 속도를 기록한다.
        void Before(Scene& scene, PhysicsSystem& physics, ObjectId owner, ComponentId mesh)
        {
            // 설정된 시나리오가 진행 중일 때만 진단용 body 변경을 허용한다.
            if (m_Kind == Kind::None || m_Done)
                return;
            PhysicsSystem::DiagAllowBodyMutation = true;

            // 공중 절단은 바닥에서 충분히 떨어진 곳에서 낙하하게 한다.
            if ((m_Kind == Kind::Midair || m_Kind == Kind::MidairSettle || m_Kind == Kind::DropNoCut) && !m_Lifted)
            {
                CheckPhysicsSim(physics.NudgeBodyForDiagnostic(owner, mesh, 3.0), "Lift cube for midair cut");
                m_Lifted = true;
                if (m_Kind == Kind::DropNoCut)
                {
                    Capture(scene, physics, owner, mesh);
                    m_Published = true;
                }
            }

            // 정지된 부모에 회전과 선속도를 한 번 주입해 회전 항을 분리한다.
            if (m_Kind == Kind::SpinPaused && m_PauseIssued && !m_SpinSeeded)
            {
                CheckPhysicsSim(!physics.IsSimulationRunning(), "Spin diagnostic runs while paused");
                CheckPhysicsSim(physics.SeedVelocityForDiagnostic(owner, mesh, { 1, -2, 0.5 }, { 0, 0, 1 }), "Seed parent diagnostic velocity");
                m_SpinSeeded = true;
            }

            // 게시가 프레임 첫 단계이므로 이전 프레임 마지막 pose가 commit 시점 pose다.
            if (m_Kind != Kind::None && !m_Published && !m_Done)
            {
                if (!m_Submitted)
                    Capture(scene, physics, owner, mesh);
                GameObject* object = scene.FindObject(owner);
                MeshComponent* parent = object ? object->FindMeshComponent(mesh) : nullptr;
                CheckPhysicsSim(parent != nullptr, "Commit snapshot parent exists");
                m_CommitWorld = parent->GetWorldTransform(object->GetTransform());
                CheckPhysicsSim(physics.GetSimulationVelocity(owner, mesh, m_Linear, m_Angular), "Parent commit velocity readable");
                CheckPhysicsSim(physics.GetSimulationCenterOfMass(owner, mesh, m_Com), "Parent commit COM readable");
            }
        }

        // 제출 결과를 보고 작업 중 깨우기와 이동을 주입한다.
        void After(Scene& scene, PhysicsSystem& physics, CuttingSystem& cutting, ObjectId owner, ComponentId mesh, uint32_t)
        {
            // 꺼져 있거나 이미 끝난 시나리오는 아무것도 하지 않는다.
            if (m_Kind == Kind::None || m_Done)
                return;

            // 작업이 도는 동안 깨운 상태를 유지한다.
            if (m_Submitted)
            {
                if (m_Kind == Kind::Woken && cutting.IsBusy())
                    CheckPhysicsSim(physics.NudgeBodyForDiagnostic(owner, mesh, 0.0), "Scenario keeps the body awake");
                return;
            }

            // 방금 제출됐다면 종류별로 개입하고 개입 뒤 상태를 기준으로 다시 기록한다.
            if (cutting.IsBusy())
            {
                m_Submitted = true;
                if (m_Kind == Kind::Woken || m_Kind == Kind::Moved)
                {
                    CheckPhysicsSim(!physics.IsSimulationAwake(owner, mesh), "Body is asleep at submit");
                    CheckPhysicsSim(physics.NudgeBodyForDiagnostic(owner, mesh, m_Kind == Kind::Moved ? 0.001 : 0.0), "Scenario disturbs the body");
                    CheckPhysicsSim(physics.IsSimulationAwake(owner, mesh), "Disturbed body is awake");
                }
                if (m_Kind == Kind::Paused || m_Kind == Kind::PausedAsleep || m_Kind == Kind::SpinPaused)
                {
                    CheckPhysicsSim(!physics.IsSimulationRunning(), "Simulation is paused at submit");
                    CheckPhysicsSim(physics.IsSimulationAwake(owner, mesh) == (m_Kind != Kind::PausedAsleep), "Paused cut has the expected sleep state");
                }
                Capture(scene, physics, owner, mesh);

                // GPU 완료가 다음 프레임에 도착해도 제출 후 실제 낙하 이동을 반드시 검사한다.
                if (m_Kind == Kind::Midair || m_Kind == Kind::MidairSettle)
                    CheckPhysicsSim(physics.AdvanceSimulation(PhysicsStepClock::FixedSeconds) == 1, "Parent falls during GPU cut");
                Before(scene, physics, owner, mesh);
                return;
            }
        }

        // 완료된 절단 결과를 시나리오 기대와 비교하고 롤백 또는 게시 상태를 검사한다.
        void OnResult(Scene& scene, PhysicsSystem& physics, const CutResultEntry& entry, uint32_t publications)
        {
            // 꺼져 있거나 이미 끝난 시나리오는 아무것도 하지 않는다.
            if (m_Kind == Kind::None || m_Done)
                return;

            // 게시 시나리오는 Published와 점프·속도·body 수를 확인하고 정착을 기다린다.
            if (m_Kind != Kind::FailA && m_Kind != Kind::FailB && m_Kind != Kind::Budget)
            {
                CheckPhysicsSim(entry.Status == CutResultStatus::Published && entry.Error == 0, "Scenario cut publishes");
                CheckPublished(scene, physics, entry);
                m_Published = true;
                m_WantResume = m_Kind == Kind::Paused || m_Kind == Kind::PausedAsleep;

                // commit 전용 시나리오는 배치와 속도 검사를 마치면 완료한다.
                if (m_Kind == Kind::Midair || m_Kind == Kind::SpinPaused)
                    Finish("commit pose and velocity inherited");
                return;
            }

            // 실패 시나리오는 지정한 이유로 끝나고 부모를 유지해야 한다.
            const char* expected = m_Kind == Kind::Budget ? "budget" : "forced cook failure";
            const bool failed = entry.Status == CutResultStatus::Failed && entry.Error == 0x200 && entry.Reason &&
                std::strstr(entry.Reason, expected) != nullptr;
            CheckPhysicsSim(failed, "Scenario cut ends with the expected rollback status");
            CheckPhysicsSim(entry.Other == InvalidComponentId && publications == 0, "Failed cut publishes nothing");
            VerifyUnchanged(scene, physics);
            Utility::Printf("Physics cut scenario detail: status=Failed error=0x%x reason=%s\n", entry.Error,
                entry.Reason ? entry.Reason : "(none)");
            Finish("failed with reason, rolled back");
        }

        // 게시 뒤 모든 body가 sleep하면 body 수·bodyless·바닥 위 정착을 한 번 검사한다.
        void Poll(Scene& scene, PhysicsSystem& physics)
        {
            // 게시된 시나리오가 실행 중이며 모두 sleep할 때까지 기다린다.
            if (!m_Published || m_Done || !physics.IsSimulationRunning() || physics.GetAwakeBodyCount() != 0)
                return;

            // 소유자의 조각을 body 보유와 bodyless로 나누고 가장 낮은 점을 구한다.
            GameObject* object = scene.FindObject(m_Owner);
            const GameObject* floor = nullptr;
            for (const auto& candidate : scene.GetObjects())
                if (candidate->GetName() == "Physics floor")
                    floor = candidate.get();
            CheckPhysicsSim(object && floor, "Scenario owner and floor exist");
            const float floorTop = PhysicsFloorTop(*floor);
            size_t withBody = 0;
            size_t bodyless = 0;
            float lowest = std::numeric_limits<float>::max();
            bool aboveFloor = true;
            for (const auto& mesh : object->GetMeshComponents())
            {
                if (mesh->IsSimulating())
                {
                    ++withBody;
                    const float pieceLowest = LowestCollisionPoint(*object, *mesh);
                    const bool asleep = !physics.IsSimulationAwake(m_Owner, mesh->GetId());
                    Utility::Printf("Physics cut scenario piece: mesh=%llu lowest=%.6f floor=%.6f asleep=%u\n",
                        mesh->GetId(), pieceLowest, floorTop, asleep ? 1u : 0u);
                    CheckPhysicsSim(asleep, "Settled piece is asleep");
                    aboveFloor = aboveFloor && pieceLowest >= floorTop - 0.01f;
                    lowest = std::min(lowest, pieceLowest);
                    continue;
                }
                ++bodyless;
                CheckPhysicsSim(mesh->GetCollision().ConvexElems.empty(), "Only a piece without convex collision stays bodyless");
            }

            // 모든 조각의 수치를 출력한 뒤 기존 관통 한계를 검사한다.
            CheckPhysicsSim(aboveFloor, "Every piece stays above the floor penetration limit");

            // 빈 쪽 수와 body 수를 확인한다.
            const bool empty = m_Kind == Kind::EmptyA || m_Kind == Kind::EmptyB;
            CheckPhysicsSim(bodyless == (empty ? 1u : 0u), "Bodyless piece count matches the scenario");
            CheckPhysicsSim(physics.GetSimulationBodyCount() == m_Snap.Bodies - 1 + withBody, "Body count is parent minus one plus pieces");

            // 적어도 하나는 바닥 ±0.01 m 안에 닿아야 한다.
            CheckPhysicsSim(std::fabs(lowest - floorTop) <= 0.01f, "At least one piece rests on the floor within 0.01 m");
            Utility::Printf("Physics cut scenario settled: bodies=%zu bodyless=%zu lowest=%.6f floor=%.6f\n", withBody, bodyless, lowest, floorTop);
            Finish("published, settled above the floor");
        }

        // 종료 시점까지 시나리오가 끝나지 않았다면 실패로 보고한다.
        void RequireDone() const { CheckPhysicsSim(m_Kind == Kind::None || m_Done, "Physics cut scenario completed before exit"); }

        // 종료 정리 단계에서 주입을 끄고, 선택된 시나리오가 끝나지 않았다면 실패로 보고한다.
        void Shutdown()
        {
            // 주입을 해제하고 종료 조건까지 포함한 검사 수를 출력한다.
            ResetInjection();
            RequireDone();
            Utility::Printf("Physics scenario checks: %zu\n", PhysicsSimCheckCount());
        }

    private:
        // 부모 메시와 장면·world 개수를 기록한다.
        void Capture(Scene& scene, const PhysicsSystem& physics, ObjectId owner, ComponentId mesh)
        {
            // 대상 메시가 있어야 기록할 수 있다.
            GameObject* object = scene.FindObject(owner);
            MeshComponent* parent = object ? object->FindMeshComponent(mesh) : nullptr;
            CheckPhysicsSim(parent != nullptr, "Scenario cut target exists");

            // 세대와 개수와 월드 배치를 저장한다.
            m_Owner = owner;
            m_Mesh = mesh;
            m_Snap.Geometry = parent->GetGeometry();
            m_Snap.Edit = parent->GetEditGeneration();
            m_Snap.Pose = parent->GetPhysicsPoseGeneration();
            m_Snap.Meshes = object->GetMeshComponents().size();
            m_Snap.Bodies = physics.GetSimulationBodyCount();
            m_Snap.Reserved = physics.GetReservedBodyCount();
            m_Snap.Awake = physics.IsSimulationAwake(owner, mesh);
            m_Snap.World = parent->GetWorldTransform(object->GetTransform());
        }

        // 실패한 절단이 부모 메시·body 수·예약 수와 pose를 바꾸지 않았는지 검사한다.
        void VerifyUnchanged(Scene& scene, const PhysicsSystem& physics) const
        {
            // 부모 메시의 geometry와 편집 세대, simulate 표시와 조각 수가 그대로여야 한다.
            GameObject* object = scene.FindObject(m_Owner);
            MeshComponent* parent = object ? object->FindMeshComponent(m_Mesh) : nullptr;
            CheckPhysicsSim(parent != nullptr, "Parent mesh survives the failed cut");
            CheckPhysicsSim(parent->GetGeometry() == m_Snap.Geometry && parent->GetEditGeneration() == m_Snap.Edit, "Parent geometry is unchanged");
            CheckPhysicsSim(parent->IsSimulating() && object->GetMeshComponents().size() == m_Snap.Meshes, "Parent still simulates and no piece was added");

            // body 수와 용량 예약 수가 시작 값으로 돌아와야 한다.
            CheckPhysicsSim(physics.GetSimulationBodyCount() == m_Snap.Bodies, "Body count is unchanged");
            CheckPhysicsSim(physics.GetReservedBodyCount() == m_Snap.Reserved, "Reservation count returns to baseline");

            // 실패 시나리오는 pose·깨어남·월드 배치도 같아야 한다.
            CheckPhysicsSim(parent->GetPhysicsPoseGeneration() == m_Snap.Pose, "Parent pose generation is unchanged");
            CheckPhysicsSim(physics.IsSimulationAwake(m_Owner, m_Mesh) == m_Snap.Awake, "Parent sleep state is unchanged");
            CheckPhysicsSim(MaxDifference(parent->GetWorldTransform(object->GetTransform()), m_Snap.World) == 0.0f, "Parent world transform is unchanged");
        }

        // 게시 직후 두 조각의 배치·COM 속도 계승·body 수를 검사한다.
        void CheckPublished(Scene& scene, const PhysicsSystem& physics, const CutResultEntry& entry) const
        {
            // 두 조각을 찾는다.
            GameObject* object = scene.FindObject(m_Owner);
            MeshComponent* a = object ? object->FindMeshComponent(m_Mesh) : nullptr;
            MeshComponent* b = object ? object->FindMeshComponent(entry.Other) : nullptr;
            CheckPhysicsSim(a && b, "Both pieces exist after publication");

            // 렌더 월드 배치가 부모의 commit 시점 배치와 1e-5 안에서 같아야 한다.
            const float jumpA = MaxDifference(a->GetWorldTransform(object->GetTransform()), m_CommitWorld);
            const float jumpB = MaxDifference(b->GetWorldTransform(object->GetTransform()), m_CommitWorld);
            CheckPhysicsSim(jumpA <= 1.0e-5f && jumpB <= 1.0e-5f, "Pieces keep the parent world transform on the publish frame");

            // body 보유 여부와 부모 COM 기준의 속도 계승을 확인한다.
            const bool emptyA = m_Kind == Kind::EmptyA;
            const bool emptyB = m_Kind == Kind::EmptyB;
            CheckPhysicsSim(a->IsSimulating() == !emptyA && b->IsSimulating() == !emptyB, "Only the non-empty pieces own bodies");
            for (const MeshComponent* piece : { a, b })
            {
                std::array<double, 3> linear;
                std::array<double, 3> angular;
                if (!piece->IsSimulating())
                    continue;
                CheckPhysicsSim(physics.GetSimulationVelocity(m_Owner, piece->GetId(), linear, angular), "Piece body velocity is readable");
                std::array<double, 3> com;
                CheckPhysicsSim(physics.GetSimulationCenterOfMass(m_Owner, piece->GetId(), com), "Piece COM readable");
                const Math::Vector3 r(float(com[0] - m_Com[0]), float(com[1] - m_Com[1]), float(com[2] - m_Com[2]));
                const Math::Vector3 w{ float(m_Angular[0]), float(m_Angular[1]), float(m_Angular[2]) };
                const Math::Vector3 rotational = Math::Cross(w, r);
                if (m_Kind == Kind::SpinPaused)
                {
                    CheckPhysicsSim(float(Math::Length(rotational)) > 0.01f, "Each piece exercises a nontrivial omega cross r");
                    Utility::Printf("Physics spin term: mesh=%llu magnitude=%g\n", piece->GetId(), float(Math::Length(rotational)));
                }
                const Math::Vector3 expected = Math::Vector3(float(m_Linear[0]), float(m_Linear[1]), float(m_Linear[2])) + rotational;
                CheckPhysicsSim(float(Math::Length(expected - Math::Vector3(float(linear[0]), float(linear[1]), float(linear[2])))) <= 1.0e-5f,
                    "Piece linear velocity matches v plus omega cross r");
                for (size_t axis = 0; axis < 3; ++axis)
                    CheckPhysicsSim(std::fabs(angular[axis] - m_Angular[axis]) <= 1.0e-5, "Piece inherits angular velocity");
            }

            // 공중 절단은 제출 후 이동과 실제 낙하 상태에서의 게시까지 확인한다.
            if (m_Kind == Kind::Midair || m_Kind == Kind::MidairSettle)
            {
                CheckPhysicsSim(m_Linear[1] < -0.2, "Parent is falling at commit");
                CheckPhysicsSim(MaxDifference(m_CommitWorld, m_Snap.World) > 1.0e-5f, "Parent moved during GPU cut");
                const GameObject* floor = nullptr;
                for (const auto& candidate : scene.GetObjects())
                    if (candidate->GetName() == "Physics floor")
                        floor = candidate.get();
                CheckPhysicsSim(floor && std::min(LowestCollisionPoint(*object, *a), LowestCollisionPoint(*object, *b)) > PhysicsFloorTop(*floor),
                    "Both pieces are above the floor at commit");
            }

            // 부모 body는 사라지고 새 body 수만큼 예약이 바뀐다.
            const size_t created = (emptyA ? 0u : 1u) + (emptyB ? 0u : 1u);
            CheckPhysicsSim(physics.GetSimulationBodyCount() == m_Snap.Bodies - 1 + created, "Body count after publication");
            CheckPhysicsSim(physics.GetReservedBodyCount() == m_Snap.Reserved - 1 + created, "Reservation count after publication");
            Utility::Printf("Physics cut scenario published: jumpA=%g jumpB=%g bodies=%zu reserved=%zu emptyA=%u emptyB=%u\n",
                jumpA, jumpB, physics.GetSimulationBodyCount(), physics.GetReservedBodyCount(), emptyA ? 1u : 0u, emptyB ? 1u : 0u);
        }

        // 두 아핀 변환의 기저·이동 성분 차이 최댓값을 구한다.
        static float MaxDifference(const Math::AffineTransform& a, const Math::AffineTransform& b)
        {
            // 네 벡터의 성분별 절댓값 차이 중 가장 큰 값을 찾는다.
            const Math::Vector3 differences[] = { a.GetX() - b.GetX(), a.GetY() - b.GetY(), a.GetZ() - b.GetZ(),
                a.GetTranslation() - b.GetTranslation() };
            float largest = 0.0f;
            for (const Math::Vector3& d : differences)
                largest = std::max({ largest, std::fabs(float(d.GetX())), std::fabs(float(d.GetY())), std::fabs(float(d.GetZ())) });
            return largest;
        }

        // body 변경 허용과 cook 실패·빈 쪽·예산 상한 주입을 기본값으로 되돌린다.
        // 여러 번 불러도 안전하다.
        void ResetInjection()
        {
            // 정적 실패 주입과 연결된 절단 시스템의 주입을 모두 끈다.
            PhysicsSystem::DiagAllowBodyMutation = false;
            CutTransaction::DiagFailSide = 0;
            if (!m_Cutting)
                return;
            m_Cutting->DiagEmptySide = -1;
            m_Cutting->GetBudget().SetLimit(CutBudget::MaxNewBodiesPerFrame);
        }

        // 시나리오를 통과로 표시하고 한 줄 결과를 남긴다.
        void Finish(const char* summary)
        {
            // 주입을 되돌린 뒤 통과로 표시한다.
            ResetInjection();
            m_Done = true;
            Utility::Printf("Physics cut scenario passed: %s\n", summary);
        }

        Kind m_Kind = Kind::None;
        CuttingSystem* m_Cutting = nullptr;
        Snapshot m_Snap;
        Math::AffineTransform m_CommitWorld;
        std::array<double, 3> m_Linear{};
        std::array<double, 3> m_Angular{};
        std::array<double, 3> m_Com{};
        bool m_Lifted = false;
        bool m_SpinSeeded = false;
        ObjectId m_Owner = InvalidObjectId;
        ComponentId m_Mesh = InvalidComponentId;
        bool m_Submitted = false;
        bool m_Published = false;
        bool m_PauseIssued = false;
        bool m_WantResume = false;
        bool m_Done = false;
    };
}
