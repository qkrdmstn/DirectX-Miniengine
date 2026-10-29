#pragma once

#include "../ObjectSelection.h"
#include "../CuttingSystem.h"
#include "Utility.h"
#include <algorithm>
#include <stdexcept>

namespace Viewer
{
    class EditDeleteChecks
    {
    public:
        // 명령줄에서 삭제 진단을 켠다.
        void Enable(bool value)
        {
            // 활성 상태를 저장한다.
            m_Enabled = value;
        }

        // 비활성이거나 완료된 진단인지 읽는다.
        bool Finished() const
        {
            // 마지막 Poll 단계가 끝났는지 확인한다.
            return !m_Enabled || m_Stage == 3;
        }

        // 게시된 A와 B의 삭제 직후 및 다음 simulation 프레임을 검사한다.
        void Poll(Scene& scene, PhysicsSystem& physics, CuttingSystem& cutting)
        {
            // 첫 게시의 두 조각과 실제 body 및 query 등록을 기다린다.
            if (Finished()) return;
            Check(physics.IsSimulationEnabled() && physics.IsSimulationRunning(), "Simulation must be running");
            if (m_Stage == 0)
            {
                if (!cutting.LastOther) return;
                m_Owner = cutting.LastOwner;
                m_B = cutting.LastOther;
                auto* object = scene.FindObject(m_Owner);
                Check(object && object->GetMeshComponents().size() == 2, "Two published pieces exist");
                for (const auto& mesh : object->GetMeshComponents())
                    if (mesh->GetId() != m_B) m_A = mesh->GetId();
                const PhysicsSystem::Key a(m_Owner, m_A), b(m_Owner, m_B);
                Check(physics.m_Bodies.count(a) && physics.m_Bodies.at(a).Handle, "A body registered");
                Check(physics.m_Bodies.count(b) && physics.m_Bodies.at(b).Handle, "B body registered");
                Check(physics.m_Entries.count(a) && physics.m_Entries.count(b), "Both query keys registered");
                m_Handle = physics.m_Bodies.at(a).Handle;
                m_BodyB = physics.m_Bodies.at(b).Handle;
                m_QueryB = physics.m_Entries.at(b).Handle;
                m_BoundsB = physics.m_Entries.at(b).Bounds;
                CheckQuery(physics, m_QueryB, m_BoundsB, true);
                m_Floor = physics.m_FloorBody;
                Check(m_Floor != MiniPhysics::InvalidBody, "Floor body exists");

                // B와 같은 owner의 A 및 다른 owner의 후보를 함께 넣는다.
                const size_t results = cutting.m_Results.size();
                Check(cutting.m_Pending.empty(), "Diagnostic starts with no pending cuts");
                auto other = physics.m_Entries.at(a).Candidate;
                other.Owner = physics.m_FloorOwner;
                other.Mesh = InvalidComponentId;
                Check(other.Owner != m_Owner, "Other candidate has a different owner");
                cutting.m_Pending.push_back(physics.m_Entries.at(b).Candidate);
                cutting.m_Pending.push_back(physics.m_Entries.at(a).Candidate);
                cutting.m_Pending.push_back(other);

                // B만 삭제하고 world 제거 및 나머지 후보 보존을 검사한다.
                m_DeletionStep = physics.GetSimulationStepCount();
                Check(DeleteEditTarget(scene, &physics, &cutting, EditTarget::MakeMesh(m_Owner, m_B)), "Delete B");
                Check(scene.FindObject(m_Owner) && !object->FindMeshComponent(m_B), "Owner survives B deletion");
                CheckAbsent(physics, m_B);
                Check(cutting.m_Pending.size() == 2 && cutting.m_Pending[0].Owner == m_Owner &&
                    cutting.m_Pending[0].Mesh == m_A && cutting.m_Pending[1].Owner == other.Owner &&
                    cutting.m_Pending[1].Mesh == other.Mesh, "Only B pending cut cancelled");
                Check(cutting.m_Results.size() == results, "Cancellation adds no result");
                CheckSurvivingA(physics);

                // 보존된 진단 후보가 다음 프레임에 제출되지 않도록 정리한다.
                cutting.m_Pending.clear();
                m_Stage = 1;
                return;
            }

            // 삭제 뒤 고정 step이 실제로 진행될 때까지 다음 검사를 미룬다.
            if (physics.GetSimulationStepCount() <= m_DeletionStep) return;

            // 다음 step과 collider 동기화 뒤에도 B가 없고 A가 살아 있는지 확인한다.
            if (m_Stage == 1)
            {
                CheckAbsent(physics, m_B);
                CheckSurvivingA(physics);

                // owner의 여러 후보와 다른 owner 후보를 넣어 바닥 삭제 거절을 검사한다.
                const auto& entry = physics.m_Entries.at({ m_Owner, m_A });
                m_QueryA = entry.Handle;
                m_BoundsA = entry.Bounds;
                CheckQuery(physics, m_QueryA, m_BoundsA, true);
                auto deleted = entry.Candidate;
                deleted.Mesh = m_B;
                auto other = entry.Candidate;
                other.Owner = physics.m_FloorOwner;
                other.Mesh = InvalidComponentId;
                const size_t results = cutting.m_Results.size();
                Check(cutting.m_Pending.empty(), "No diagnostic candidates remain");
                cutting.m_Pending.push_back(entry.Candidate);
                cutting.m_Pending.push_back(deleted);
                cutting.m_Pending.push_back(other);
                Check(!DeleteEditTarget(scene, &physics, &cutting, EditTarget::MakeOwner(physics.m_FloorOwner)), "Floor deletion rejected");
                Check(cutting.m_Pending.size() == 3 && cutting.m_Pending[0].Owner == m_Owner &&
                    cutting.m_Pending[0].Mesh == m_A && cutting.m_Pending[1].Owner == m_Owner &&
                    cutting.m_Pending[1].Mesh == m_B && cutting.m_Pending[2].Owner == other.Owner &&
                    cutting.m_Pending[2].Mesh == other.Mesh, "Rejected deletion preserves pending cuts");

                // owner를 삭제하고 해당 owner의 모든 후보가 제거되는지 검사한다.
                m_DeletionStep = physics.GetSimulationStepCount();
                Check(DeleteEditTarget(scene, &physics, &cutting, EditTarget::MakeOwner(m_Owner)), "Delete owner");
                Check(cutting.m_Pending.size() == 1 && cutting.m_Pending[0].Owner == other.Owner &&
                    cutting.m_Pending[0].Mesh == other.Mesh, "Only owner pending cuts cancelled");
                Check(cutting.m_Results.size() == results, "Owner cancellation adds no result");
                Check(!scene.FindObject(m_Owner), "Owner removed from scene");
                CheckAbsent(physics, InvalidComponentId);
                Check(physics.m_FloorBody == m_Floor, "Floor handle preserved immediately");

                // 다른 owner의 진단 후보도 직접 정리해 이후 게시 수를 보존한다.
                cutting.m_Pending.clear();
                m_Stage = 2;
                return;
            }

            // owner 삭제 다음 프레임에도 재등록이 없고 바닥 body가 살아 있는지 확인한다.
            CheckAbsent(physics, InvalidComponentId);
            std::array<double, 3> position;
            std::array<double, 4> rotation;
            Check(physics.m_FloorBody == m_Floor && physics.m_World.GetBodyPose(m_Floor, position, rotation), "Floor body survives");
            m_Stage = 3;
            Utility::Printf("Edit delete validation passed\n");
        }

    private:
        // A의 handle과 world의 실제 body가 모두 보존되었는지 검사한다.
        void CheckSurvivingA(const PhysicsSystem& physics) const
        {
            // handle 비교와 pose 조회로 등록 맵과 world를 함께 확인한다.
            std::array<double, 3> position;
            std::array<double, 4> rotation;
            Check(physics.m_Bodies.at({ m_Owner, m_A }).Handle == m_Handle &&
                physics.m_World.GetBodyPose(m_Handle, position, rotation), "A body survives in world");
        }

        // 저장한 경계에서 query handle의 실제 world 존재 여부를 검사한다.
        static void CheckQuery(const PhysicsSystem& physics, MiniPhysics::ColliderHandle handle,
            const MiniPhysics::QueryBounds& bounds, bool present)
        {
            // 삭제 전에도 같은 조회가 성공해야 삭제 후 부재 검사가 유효하다.
            Check(handle != MiniPhysics::InvalidCollider, "Query handle registered");
            const auto hits = physics.m_World.QueryAabb(bounds);
            Check((std::find(hits.begin(), hits.end(), handle) != hits.end()) == present, "Query world presence matches");
        }

        // 삭제 대상의 물리 인덱스와 실제 world에 참조가 남지 않았는지 검사한다.
        void CheckAbsent(const PhysicsSystem& physics, ComponentId mesh) const
        {
            // 삭제 전에 저장한 handle로 실제 simulation body와 query collider를 확인한다.
            std::array<double, 3> position;
            std::array<double, 4> rotation;
            Check(!physics.m_World.GetBodyPose(m_BodyB, position, rotation), "Deleted B body absent from world");
            CheckQuery(physics, m_QueryB, m_BoundsB, false);
            if (mesh == InvalidComponentId)
            {
                Check(!physics.m_World.GetBodyPose(m_Handle, position, rotation), "Deleted A body absent from world");
                CheckQuery(physics, m_QueryA, m_BoundsA, false);
            }

            // simulation과 query 및 역방향 인덱스와 fallback 목록을 각각 확인한다.
            for (const auto& entry : physics.m_Bodies)
                Check(entry.first.first != m_Owner || (mesh && entry.first.second != mesh), "Deleted simulation key absent");
            for (const auto& entry : physics.m_Entries)
                Check(entry.first.first != m_Owner || (mesh && entry.first.second != mesh), "Deleted query key absent");
            for (const auto& entry : physics.m_Handles)
                Check(entry.second.first != m_Owner || (mesh && entry.second.second != mesh), "Deleted reverse handle absent");
            for (const auto& key : physics.m_FallbackKeys)
                Check(key.first != m_Owner || (mesh && key.second != mesh), "Deleted fallback key absent");
        }

        // Release에서도 진단 실패를 로그와 예외로 전달한다.
        static void Check(bool condition, const char* message)
        {
            // 실패한 조건만 출력하고 실행을 중단한다.
            if (!condition)
            {
                Utility::Printf("Edit delete check failed: %s\n", message);
                throw std::runtime_error(message);
            }
        }

        bool m_Enabled = false;
        unsigned m_Stage = 0;
        uint64_t m_DeletionStep = 0;
        ObjectId m_Owner = InvalidObjectId;
        ComponentId m_A = InvalidComponentId, m_B = InvalidComponentId;
        MiniPhysics::BodyHandle m_BodyB = MiniPhysics::InvalidBody;
        MiniPhysics::ColliderHandle m_QueryA = MiniPhysics::InvalidCollider, m_QueryB = MiniPhysics::InvalidCollider;
        MiniPhysics::QueryBounds m_BoundsA{}, m_BoundsB{};
        MiniPhysics::BodyHandle m_Handle = MiniPhysics::InvalidBody, m_Floor = MiniPhysics::InvalidBody;
    };
}
