#pragma once

#include "../ObjectSelection.h"
#include "Utility.h"
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace Viewer
{
    // Release에서도 편집 대상 진단 실패를 예외로 보고한다.
    inline void CheckEditTarget(bool condition, const char* message)
    {
        // false 조건을 런타임 실패로 전달한다.
        if (!condition)
        {
            Utility::Printf("Edit target check failed: %s\n", message);
            throw std::runtime_error(message);
        }
    }

    // 장면이 준비되면 한 번, 모든 메시를 프로그램으로 pick하여 R8 편집 대상 규칙을 검사한다.
    class EditTargetChecks
    {
    public:
        // -edit-target-checks 1일 때 진단을 켠다.
        void Enable(bool value) { m_Enabled = value; }
        // 진단이 꺼져 있거나 이미 끝났는지 읽는다.
        bool Finished() const { return !m_Enabled || m_Done; }

        // 모든 메시의 pose 경계를 검사하고 선택 출력이 있으면 실제 overlay 렌더 대상도 지정한다.
        void Poll(Scene& scene, EditTarget* selected = nullptr)
        {
            // 꺼졌거나 이미 끝났으면 건너뛴다.
            if (Finished())
                return;

            // 아직 pose 경계가 없는 메시가 있으면 다음 프레임으로 미룬다.
            for (const auto& object : scene.GetObjects())
                for (const auto& mesh : object->GetMeshComponents())
                    if (!mesh->GetGeometry()->GetSections().empty() && !mesh->HasPoseBounds())
                        return;

            // 메시마다 구 바깥 26방향 ray로 pick하여 대상 종류와 기즈모 위치를 확인한다.
            unsigned meshTargets = 0;
            unsigned ownerTargets = 0;
            std::vector<std::pair<ObjectId, ComponentId>> seen;
            for (const auto& object : scene.GetObjects())
            {
                if (!object->IsEditable())
                    continue;
                for (const auto& mesh : object->GetMeshComponents())
                {
                    if (mesh->GetGeometry()->GetSections().empty())
                        continue;
                    PickHit hit;
                    CheckEditTarget(PickMesh(scene, *object, *mesh, hit), "Every piece can be picked from some direction");
                    CheckPick(scene, *object, *mesh, hit, meshTargets, ownerTargets, seen);

                    // 명시적으로 켠 진단에서만 첫 생성 조각을 실제 화면 선택으로 연결한다.
                    if (selected && selected->IsNone() && mesh->GetGeometry()->IsGenerated())
                        *selected = ResolveEditTarget(scene, hit);
                }
            }
            // 장면이 보장하는 범주가 비어 있으면 통과하지 않는다. simulate 장면은 Mesh 대상, sim off 장면은 Owner 대상이 있어야 한다.
            bool hasFloor = false;
            for (const auto& object : scene.GetObjects())
                hasFloor = hasFloor || !object->IsEditable();
            CheckEditTarget(meshTargets + ownerTargets > 0, "At least one piece was picked");
            CheckEditTarget(hasFloor ? meshTargets >= 1 : ownerTargets >= 1, "Expected target category is not empty");

            // 바닥은 어느 방향에서도 pick되지 않아야 한다.
            const unsigned floors = CheckFloorsNotPickable(scene);

            // 사라진 대상은 무효가 되어야 한다.
            CheckInvalidation(scene);
            m_Done = true;
            Utility::Printf("Edit target validation passed: meshTargets=%u ownerTargets=%u floors=%u\n",
                meshTargets, ownerTargets, floors);
        }

    private:
        // 구 중심을 향한 26방향 ray 중 이 메시를 맞히는 첫 결과를 찾는다.
        static bool PickMesh(const Scene& scene, const GameObject& object, const MeshComponent& mesh, PickHit& found)
        {
            // 방향 격자 {-1,0,1}^3에서 0이 아닌 방향을 모두 시도한다.
            const Math::BoundingSphere bounds = mesh.GetBoundingSphere(object.GetTransform());
            const float distance = float(bounds.GetRadius()) * 3.0f + 1.0f;
            for (int x = -1; x <= 1; ++x)
            {
                for (int y = -1; y <= 1; ++y)
                {
                    for (int z = -1; z <= 1; ++z)
                    {
                        if (!x && !y && !z)
                            continue;
                        const Math::Vector3 direction = Math::Normalize(Math::Vector3(float(x), float(y), float(z)));
                        const PickingRay ray = { bounds.GetCenter() + direction * distance, -direction };
                        const PickHit hit = PickObject(scene, ray);
                        if (hit.Id == object.GetId() && hit.MeshId == mesh.GetId())
                        {
                            found = hit;
                            return true;
                        }
                    }
                }
            }
            return false;
        }

        // pick 결과가 R8 규칙의 대상 종류, 유효성, 기즈모 위치, 고유성을 만족하는지 검사한다.
        static void CheckPick(const Scene& scene, const GameObject& object, const MeshComponent& mesh, const PickHit& hit,
            unsigned& meshTargets, unsigned& ownerTargets, std::vector<std::pair<ObjectId, ComponentId>>& seen)
        {
            // 월드 독립이면 Mesh, 아니면 Owner여야 한다.
            const bool independent = mesh.GetTransformMode() == MeshTransformMode::WorldIndependent;
            const EditTarget target = ResolveEditTarget(scene, hit);
            CheckEditTarget(target.IsMesh() == independent, "WorldIndependent picks Mesh and Attached picks Owner");
            CheckEditTarget(target.Object == object.GetId(), "Target owner matches the picked owner");
            CheckEditTarget(IsEditTargetValid(scene, target), "Resolved target is valid");

            // 기즈모 위치는 Mesh는 조각의 월드 위치, Owner는 owner 위치와 같아야 한다.
            SceneTransform frame;
            CheckEditTarget(TryGetEditTargetTransform(scene, target, frame), "Target has a gizmo transform");
            const Math::Vector3 expected = independent ? mesh.GetTransform().GetTranslation() : object.GetTransform().GetTranslation();
            CheckEditTarget(float(Math::Length(frame.GetTranslation() - expected)) < 1e-4f, "Gizmo is placed at the target world position");

            // 절단 조각의 표시 중심은 편집 원점과 분리하고 최신 경계 중심을 따라야 한다.
            const Math::Vector3 pivot = GetEditTargetPivot(scene, target, frame);
            const Math::Vector3 expectedPivot = independent && mesh.GetGeometry()->IsGenerated() ?
                mesh.GetBoundingSphere(object.GetTransform()).GetCenter() : expected;
            CheckEditTarget(float(Math::Length(pivot - expectedPivot)) < 1e-4f, "Generated piece gizmo follows its bounds center");

            // 서로 다른 조각은 서로 다른 Mesh 대상이어야 한다.
            if (independent)
            {
                const std::pair<ObjectId, ComponentId> key(target.Object, target.Mesh);
                for (const auto& previous : seen)
                    CheckEditTarget(previous != key, "Pieces resolve to distinct Mesh targets");
                seen.push_back(key);
                ++meshTargets;
            }
            else
            {
                CheckEditTarget(target == EditTarget::MakeOwner(object.GetId()), "Attached picks resolve to the owner target");
                ++ownerTargets;
            }
        }

        // 편집 잠금 객체(바닥)가 어느 방향에서도 pick되지 않는지 검사하고 개수를 반환한다.
        static unsigned CheckFloorsNotPickable(const Scene& scene)
        {
            // 잠긴 객체의 경계 구 중심을 향해 26방향으로 ray를 쏜다.
            unsigned floors = 0;
            for (const auto& object : scene.GetObjects())
            {
                if (object->IsEditable())
                    continue;
                ++floors;
                for (const auto& mesh : object->GetMeshComponents())
                {
                    if (mesh->GetGeometry()->GetSections().empty() || !mesh->HasPoseBounds())
                        continue;
                    const Math::BoundingSphere bounds = mesh->GetBoundingSphere(object->GetTransform());
                    const float distance = float(bounds.GetRadius()) * 3.0f + 1.0f;
                    for (int x = -1; x <= 1; ++x)
                    {
                        for (int y = -1; y <= 1; ++y)
                        {
                            for (int z = -1; z <= 1; ++z)
                            {
                                if (!x && !y && !z)
                                    continue;
                                const Math::Vector3 direction = Math::Normalize(Math::Vector3(float(x), float(y), float(z)));
                                const PickHit hit = PickObject(scene, { bounds.GetCenter() + direction * distance, -direction });
                                CheckEditTarget(hit.Id != object->GetId(), "Locked floor is never picked");
                            }
                        }
                    }
                }
            }
            return floors;
        }

        // 존재하지 않거나 삭제된 대상이 무효로 판정되는지 검사한다.
        static void CheckInvalidation(Scene& scene)
        {
            // 없는 owner와 없는 컴포넌트는 무효다.
            CheckEditTarget(!IsEditTargetValid(scene, EditTarget::MakeOwner(0xFFFFFFFFull)), "Unknown owner is invalid");
            CheckEditTarget(!IsEditTargetValid(scene, EditTarget()), "Empty target is invalid");
            if (!scene.GetObjects().empty())
                CheckEditTarget(!IsEditTargetValid(scene, EditTarget::MakeMesh(scene.GetObjects().front()->GetId(), 0xFFFFFFFFu)),
                    "Unknown component is invalid");

            // 임시 객체를 지우면 그 owner 대상은 무효가 된다.
            const ObjectId probe = scene.CreateObject("EditTargetProbe").GetId();
            CheckEditTarget(IsEditTargetValid(scene, EditTarget::MakeOwner(probe)), "Live owner is valid");
            CheckEditTarget(scene.DeleteObject(probe), "Probe object is deleted");
            CheckEditTarget(!IsEditTargetValid(scene, EditTarget::MakeOwner(probe)), "Deleted owner target is invalid");
        }

        bool m_Enabled = false;
        bool m_Done = false;
    };
}
