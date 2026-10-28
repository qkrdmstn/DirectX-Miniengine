#pragma once

#include "EditTargetChecks.h"
#include "../PhysicsSystem.h"
#include <cstring>

namespace Viewer
{
    class EditApplyChecks
    {
    public:
        // 명령줄에서 공통 편집 적용 진단을 활성화한다.
        void Enable(bool value) { m_Enabled = value; }
        // 비활성 또는 완료된 진단인지 읽는다.
        bool Finished() const { return !m_Enabled || m_Done; }

        // sleep한 첫 조각의 편집과 이력 복구 및 다음 프레임의 body 유지를 확인한다.
        void Poll(Scene& scene, PhysicsSystem& physics)
        {
            // 비활성이면 건너뛰고 각 편집 다음 프레임에 같은 handle인지 검사한다.
            if (Finished()) return;
            if (m_Handle)
            {
                const auto entry = physics.m_Bodies.find({ m_Target.Object, m_Target.Mesh });
                Check(entry != physics.m_Bodies.end() && entry->second.Handle == m_Handle, "Handle survives next frame");
                auto* object = scene.FindObject(m_Target.Object);
                auto* mesh = object ? object->FindMeshComponent(m_Target.Mesh) : nullptr;
                Check(mesh != nullptr, "Edited mesh survives next frame");

                // 이동·회전 이후 프레임에는 크기만 편집하고 다음 Poll을 기다린다.
                if (!m_ScaleApplied)
                {
                    ApplyScale(scene, physics, *mesh);
                    m_ScaleApplied = true;
                    return;
                }

                // 크기 편집 이후 프레임에는 이력을 복구하고 다음 Poll을 기다린다.
                if (!m_UndoApplied)
                {
                    CheckHistoryUndo(scene, physics, *mesh);
                    m_UndoApplied = true;
                    return;
                }

                // Undo 이후 프레임의 handle 유지와 실패 원자성 및 바닥 잠금을 확인하고 완료한다.
                CheckRejections(scene, physics, *mesh);
                m_Done = true;
                Utility::Printf("Edit apply validation passed\n");
                return;
            }

            // 실제 실행 중인 simulation에서 바닥에 정착한 첫 Mesh를 기다린다.
            Check(physics.IsSimulationEnabled() && physics.IsSimulationRunning(), "Simulation must be running");
            for (const auto& object : scene.GetObjects())
                for (const auto& mesh : object->GetMeshComponents())
                    if (object->IsEditable() && physics.GetBodyState(object->GetId(), mesh->GetId()) == PhysicsSystem::BodyState::Asleep)
                    {
                        m_Target = EditTarget::MakeMesh(object->GetId(), mesh->GetId());
                        Run(scene, physics, *mesh);
                        return;
                    }
        }

    private:
        // 실제 이력 적용 콜백으로 이동을 취소하고 같은 body의 원점 복구를 검사한다.
        void CheckHistoryUndo(Scene& scene, PhysicsSystem& physics, MeshComponent& mesh)
        {
            // 현재 월드 TRS에서 이동 편집을 만든 뒤 전후 값을 기록한다.
            SceneTransform before;
            Check(TryGetEditTargetTransform(scene, m_Target, before), "Read history Before");
            SceneTransform moved = before;
            moved.SetTranslation(before.GetTranslation() + Math::Vector3(0.2f, 0.3f, 0.1f));
            Check(ApplyEditTransform(scene, &physics, m_Target, moved), "Apply history translation");
            SceneTransform after;
            Check(TryGetEditTargetTransform(scene, m_Target, after), "Read history After");
            TransformHistory history;
            Check(history.Record(m_Target, HistoryTransformMode::WorldIndependent, before, after), "Record mesh history");
            const uint64_t generation = mesh.GetEditGeneration();

            // 기즈모와 같은 콜백으로 이전 상태를 복구한다.
            Check(history.Undo([&](const TransformHistory::Command& command)
            {
                // 공통 경로가 body까지 순간이동하도록 전달한다.
                return ApplyHistoryCommand(scene, &physics, command);
            }), "Undo mesh history");
            Check(mesh.GetEditGeneration() == generation + 1, "Undo advances generation once");
            Check(physics.m_Bodies.at({ m_Target.Object, m_Target.Mesh }).Handle == m_Handle, "Undo preserves body");

            // 복구 직후 body 원점과 렌더 위치가 Before인지 검사한다.
            std::array<double, 3> position;
            std::array<double, 4> rotation;
            Check(physics.m_World.GetBodyPose(m_Handle, position, rotation), "Read undo body pose");
            DirectX::XMFLOAT3 expected;
            DirectX::XMStoreFloat3(&expected, before.GetTranslation());
            const double expectedP[] = { expected.x, expected.y, expected.z };
            for (unsigned i = 0; i < 3; ++i)
                Check(std::abs(position[i] - expectedP[i]) <= 1e-5, "Undo body translation matches Before");
            Check(float(Math::Length(mesh.GetTransform().GetTranslation() - before.GetTranslation())) <= 1e-5f,
                "Undo render translation matches Before");
        }

        // Release에서도 실패 이유를 출력한 뒤 예외로 중단한다.
        static void Check(bool condition, const char* message)
        {
            // 실패만 로그와 예외로 전달한다.
            if (!condition)
            {
                Utility::Printf("Edit apply check failed: %s\n", message);
                throw std::runtime_error(message);
            }
        }

        // 이동·회전을 적용하고 pose와 속도 및 편집 세대를 즉시 검사한다.
        void Run(Scene& scene, PhysicsSystem& physics, MeshComponent& mesh)
        {
            // 편집 직전 handle과 세대 및 선속도를 보관한다.
            using namespace Math;
            auto& entry = physics.m_Bodies.at({ m_Target.Object, m_Target.Mesh });
            m_Handle = entry.Handle;
            const uint64_t generation = mesh.GetEditGeneration();
            std::array<double, 3> before, after, angular, position;
            std::array<double, 4> rotation;
            Check(physics.GetSimulationVelocity(m_Target.Object, m_Target.Mesh, before, angular), "Read velocity before edit");
            SceneTransform world = mesh.GetTransform();
            world.SetTranslation(world.GetTranslation() + Vector3(0.2f, 0.3f, 0.1f));
            world.SetRotation(Quaternion(Vector3(kYUnitVector), 0.3f) * world.GetRotation());
            Check(world.Normalize(), "Normalize request");

            // 순간이동은 원점 pose와 세대만 바꾸고 선속도와 handle은 유지한다.
            Check(ApplyEditTransform(scene, &physics, m_Target, world), "Apply translation and rotation");
            Check(mesh.GetEditGeneration() == generation + 1 && entry.Handle == m_Handle, "Generation advances once without replacement");
            Check(physics.GetSimulationVelocity(m_Target.Object, m_Target.Mesh, after, angular) && before == after, "Linear velocity preserved");
            Check(physics.m_World.GetBodyPose(m_Handle, position, rotation), "Read body origin pose");
            DirectX::XMFLOAT3 p;
            DirectX::XMFLOAT4 q;
            DirectX::XMStoreFloat3(&p, world.GetTranslation());
            DirectX::XMStoreFloat4(&q, world.GetRotation());
            const double expectedP[] = { p.x, p.y, p.z }, expectedQ[] = { q.x, q.y, q.z, q.w };
            for (unsigned i = 0; i < 3; ++i) Check(std::abs(position[i] - expectedP[i]) <= 1e-5, "Body origin translation matches");
            const double sign = rotation[3] * expectedQ[3] < 0 ? -1.0 : 1.0;
            for (unsigned i = 0; i < 4; ++i) Check(std::abs(sign * rotation[i] - expectedQ[i]) <= 1e-5, "Body origin rotation matches");
        }

        // 크기를 편집하고 렌더와 실제 solver 질량에 반영되는지 검사한다.
        void ApplyScale(Scene& scene, PhysicsSystem& physics, MeshComponent& mesh)
        {
            // 현재 프레임의 TRS에서 크기만 늘리고 같은 body의 질량을 비교한다.
            using namespace Math;
            auto& entry = physics.m_Bodies.at({ m_Target.Object, m_Target.Mesh });
            SceneTransform world = mesh.GetTransform();
            double oldMass, mass;
            Check(physics.m_World.GetBodyMass(m_Handle, oldMass), "Read mass before scale");
            world.SetScale(world.GetScale() * 1.5f);
            Check(ApplyEditTransform(scene, &physics, m_Target, world), "Apply scale");
            Check(float(Length(mesh.GetTransform().GetScale() - world.GetScale())) == 0, "Render scale updated");
            Check(physics.m_World.GetBodyMass(m_Handle, mass) && mass > oldMass && entry.Handle == m_Handle, "Mass increases on same body");
        }

        // cook 실패의 원자성과 바닥 편집 거절을 검사한다.
        void CheckRejections(Scene& scene, PhysicsSystem& physics, MeshComponent& mesh)
        {
            // 실패 전 질량과 현재 프레임의 TRS를 보관한다.
            using namespace Math;
            double oldMass, mass;
            Check(physics.m_World.GetBodyMass(m_Handle, mass), "Read mass before rejected edit");
            SceneTransform world = mesh.GetTransform();

            // 첫 hull의 정점 수를 잠시 세 개로 줄여 크기와 무관하게 cook을 실패시킨다.
            const SceneTransform saved = mesh.GetTransform();
            const uint64_t savedGeneration = mesh.GetEditGeneration();
            auto& collision = const_cast<MiniCollision::AggregateGeom&>(mesh.GetCollision());
            auto vertices = collision.ConvexElems.front().VertexData;
            collision.ConvexElems.front().VertexData.resize(3);
            world.SetScale(Vector3(0.0001f, 0.0001f, 0.0001f));
            const bool accepted = ApplyEditTransform(scene, &physics, m_Target, world);
            collision.ConvexElems.front().VertexData.swap(vertices);
            Check(!accepted, "Malformed hull cook rejected");
            Check(mesh.GetEditGeneration() == savedGeneration, "Failed cook preserves generation");
            Check(float(Length(mesh.GetTransform().GetTranslation() - saved.GetTranslation())) == 0 &&
                float(Length(mesh.GetTransform().GetScale() - saved.GetScale())) == 0 &&
                std::memcmp(&mesh.GetTransform(), &saved, sizeof(saved)) == 0, "Failed cook preserves render TRS");
            Check(physics.m_World.GetBodyMass(m_Handle, oldMass) && oldMass == mass, "Failed cook preserves mass");

            // 잠긴 바닥은 Owner와 Mesh 대상 모두에서 편집을 거절해야 한다.
            Check(physics.m_FloorOwner != InvalidObjectId, "Floor exists");
            Check(!ApplyEditTransform(scene, &physics, EditTarget::MakeOwner(physics.m_FloorOwner), world), "Floor owner locked");
            Check(!ApplyEditTransform(scene, &physics, EditTarget::MakeMesh(physics.m_FloorOwner, physics.m_FloorMesh), world), "Floor mesh locked");
        }

        bool m_Enabled = false;
        bool m_Done = false;
        bool m_ScaleApplied = false;
        bool m_UndoApplied = false;
        EditTarget m_Target;
        MiniPhysics::BodyHandle m_Handle = MiniPhysics::InvalidBody;
    };
}
