#pragma once

#include "../PhysicsSystem.h"
#include "Utility.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Viewer
{
    // Release에서도 물리 장면 진단 실패를 예외로 보고한다.
    inline void CheckPhysicsSim(bool condition, const char* message)
    {
        // false 조건을 호출 위치와 무관하게 런타임 실패로 전달한다.
        if (!condition)
            throw std::runtime_error(message);
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
                CheckPhysicsSim(!object->IsEditable(), "Simulating owner editing is temporarily blocked");
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
}
