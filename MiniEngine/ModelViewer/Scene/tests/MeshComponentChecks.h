#pragma once

#include "../Scene.h"
#include "Renderer.h"
#include "Utility.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace Viewer
{
    // Release에서도 실패를 숨기지 않는 GPU 통합 진단의 조건을 검사한다.
    inline void CheckComponent(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
    }

    // 선택 section이 모두 Generated일 때만 Model hull 부피 합과 convex·정점 수를 센다. 하나라도 아니면 모두 0이다.
    inline void SumModelHulls(const GeometryRevision& revision, double& volume, size_t& convex, size_t& vertices)
    {
        // 합계를 비운다.
        volume = 0.0;
        convex = vertices = 0;

        // 선택 section의 hull을 누적하고, Generated가 아닌 section을 만나면 전체를 0으로 되돌린다.
        for (const GeometrySection& section : revision.GetSections())
        {
            const Model& model = *revision.GetSource();
            if (model.GetMeshCollisionStatus(section.SourceMesh) != MeshCollisionStatus::Generated)
            {
                volume = 0.0;
                convex = vertices = 0;
                return;
            }
            for (const MiniCollision::ConvexElem& elem : model.GetMeshCollision(section.SourceMesh).ConvexElems)
            {
                volume += elem.GetVolume();
                ++convex;
                vertices += elem.VertexData.size();
            }
        }
    }

    // 모든 hull 정점을 SourceFrame 역변환(켤레 쿼터니언 회전 후 크기 나눗셈, double)과 비교한다. 허용오차는 extent 대비 1e-9다.
    inline void CheckCollisionAgainstFrame(const MeshComponent& mesh, const SceneTransform& frame)
    {
        // 프레임의 TRS를 double로 꺼낸다.
        DirectX::XMFLOAT4 q;
        DirectX::XMFLOAT3 s, t;
        XMStoreFloat4(&q, frame.GetRotation());
        XMStoreFloat3(&s, frame.GetScale());
        XMStoreFloat3(&t, frame.GetTranslation());
        const double n = std::sqrt(double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w);
        const double u[3] = { -q.x / n, -q.y / n, -q.z / n };
        const double w = q.w / n;

        // 선택 section의 Model hull을 같은 순서로 순회하며 컴포넌트 정점과 대조한다.
        const Model& model = *mesh.GetGeometry()->GetSource();
        const auto& elems = mesh.GetCollision().ConvexElems;
        size_t index = 0;
        for (const GeometrySection& section : mesh.GetGeometry()->GetSections())
        {
            if (model.GetMeshCollisionStatus(section.SourceMesh) != MeshCollisionStatus::Generated)
                continue;
            for (const MiniCollision::ConvexElem& source : model.GetMeshCollision(section.SourceMesh).ConvexElems)
            {
                CheckComponent(index < elems.size() && elems[index].VertexData.size() == source.VertexData.size(), "Frame hull shape");
                double extent = 1e-30;
                for (size_t i = 0; i < source.VertexData.size(); ++i)
                {
                    // p - T를 켤레 쿼터니언으로 회전한다: v' = v + 2w(u×v) + 2u×(u×v).
                    const double v[3] = { source.VertexData[i].x - t.x, source.VertexData[i].y - t.y, source.VertexData[i].z - t.z };
                    const double c1[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
                    const double c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
                    const double expected[3] = { (v[0] + 2 * (w * c1[0] + c2[0])) / s.x,
                        (v[1] + 2 * (w * c1[1] + c2[1])) / s.y, (v[2] + 2 * (w * c1[2] + c2[2])) / s.z };
                    const MiniCollision::Vec3d& actual = elems[index].VertexData[i];
                    extent = std::max({ extent, std::abs(expected[0]), std::abs(expected[1]), std::abs(expected[2]) });
                    CheckComponent(std::abs(actual.x - expected[0]) <= 1e-9 * extent
                        && std::abs(actual.y - expected[1]) <= 1e-9 * extent
                        && std::abs(actual.z - expected[2]) <= 1e-9 * extent, "Frame hull vertex");
                }
                ++index;
            }
        }
        CheckComponent(index == elems.size(), "Frame hull count");
    }

    // 컴포넌트 충돌이 Model hull과 일치하고 항상 현재 revision에 대응하는지 검사한다.
    inline void CheckComponentCollision(const MeshComponent& mesh, double scaleVolume, double tolerance)
    {
        // 현재 revision의 Model hull 합계를 구한다.
        double volume = 0.0;
        size_t convex = 0, vertices = 0;
        SumModelHulls(*mesh.GetGeometry(), volume, convex, vertices);

        // revision 대응과 상태를 확인한다.
        CheckComponent(mesh.GetCollisionRevisionId() == mesh.GetGeometry()->GetId(), "Collision revision id");
        if (convex == 0)
        {
            CheckComponent(mesh.GetCollisionStatus() == ComponentCollisionStatus::Unavailable
                && mesh.GetCollision().ConvexElems.empty(), "Collision unavailable without model hull");
            return;
        }
        CheckComponent(mesh.GetCollisionStatus() == ComponentCollisionStatus::FromModel, "Collision from model");
        CheckComponent(mesh.GetCollision().ConvexElems.size() == convex, "Collision convex count");

        // 부피와 정점 수가 기대값과 일치하는지 확인한다.
        double actual = 0.0;
        size_t actualVertices = 0;
        for (const auto& elem : mesh.GetCollision().ConvexElems)
        {
            actual += elem.GetVolume();
            actualVertices += elem.VertexData.size();
        }
        CheckComponent(std::abs(actual - volume * scaleVolume) <= tolerance * volume * scaleVolume, "Collision volume");
        CheckComponent(actualVertices == vertices, "Collision vertex count");
    }

    // 가짜 상태 목록으로 혼합·범위 밖·정상 section 선택의 all-or-nothing 결과를 검사한다. Generated hull이 없는 모델은 건너뛴다.
    inline void CheckMixedSectionStatus(const Model& model)
    {
        // 첫 Generated hull을 찾아 두 메시의 hull로 복제한다.
        const MiniCollision::AggregateGeom* hull = nullptr;
        for (uint32_t i = 0; i < model.m_NumMeshes && !hull; ++i)
            if (model.GetMeshCollisionStatus(i) == MeshCollisionStatus::Generated)
                hull = &model.GetMeshCollision(i);
        if (!hull)
            return;
        const std::vector<MiniCollision::AggregateGeom> hulls = { *hull, *hull };
        const double identity[3][4] = { {1,0,0,0}, {0,1,0,0}, {0,0,1,0} };
        MiniCollision::AggregateGeom out;
        std::string reason;

        // 모두 Generated이면 두 hull이 모인다.
        const std::vector<MeshCollisionStatus> allGenerated = { MeshCollisionStatus::Generated, MeshCollisionStatus::Generated };
        CheckComponent(GatherSectionHulls(allGenerated, hulls, { {0, 0}, {1, 0} }, identity, out, reason)
            && out.ConvexElems.size() == 2 * hull->ConvexElems.size(), "All generated sections gathered");

        // 스킨 section이 하나라도 섞이면 결과가 비고 이유에 section과 상태가 있어야 한다.
        const std::vector<MeshCollisionStatus> mixed = { MeshCollisionStatus::Generated, MeshCollisionStatus::Skinned };
        CheckComponent(!GatherSectionHulls(mixed, hulls, { {0, 0}, {1, 0} }, identity, out, reason)
            && out.ConvexElems.empty() && reason.find("section 1") != std::string::npos
            && reason.find("Skinned") != std::string::npos, "Mixed sections unavailable");

        // 범위 밖 section도 결과를 비운다.
        CheckComponent(!GatherSectionHulls(mixed, hulls, { {0, 0}, {5, 0} }, identity, out, reason)
            && out.ConvexElems.empty(), "Out of range section unavailable");
    }

    // ID·revision·collider·변환 계약을 검사하고 같은 owner 아래 두 번째 메시를 배치한다.
    inline void PrepareMeshComponentChecks(Scene& scene)
    {
        using namespace Math;
        size_t checked = 0;
        for (const auto& object : scene.GetObjects())
        {
            if (!object->GetModelInstance())
                continue;

            // owner의 첫 컴포넌트와 원본 revision을 잡고 소유권을 확인한다.
            const ModelInstance* pose = object->GetModelInstance();
            MeshComponent& first = *object->GetMeshComponents().front();
            const auto original = first.GetGeometry();
            const ComponentId firstId = first.GetId();
            CheckComponent(original->GetSource() == pose->GetModel(), "Source ownership");

            // collider 대응과 초기 충돌 상태를 확인한다.
            const ComponentId colliderId = object->AddColliderComponent(firstId).Id;
            CheckComponent(object->IsColliderCurrent(colliderId), "Initial collider revision");
            CheckComponentCollision(first, 1.0, 1e-9);

            // 빈 revision으로 바꾸면 collider가 낡고 hull이 남지 않아야 한다.
            const auto empty = original->SelectSections({});
            CheckComponent(empty->GetId() != original->GetId(), "Revision identity");
            CheckComponent(first.SetGeometry(empty), "Geometry replacement");
            CheckComponent(!object->IsColliderCurrent(colliderId), "Stale collider rejected");
            CheckComponent(first.GetCollisionStatus() == ComponentCollisionStatus::Unavailable
                && first.GetCollision().ConvexElems.empty(), "Empty revision has no stale hull");

            // 원본으로 복원하면 collider와 충돌이 다시 현재 revision에 대응해야 한다.
            CheckComponent(first.SetGeometry(original), "Geometry snapshot restore");
            CheckComponentCollision(first, 1.0, 1e-9);
            if (original->GetSections().size() > 1)
            {
                const auto subset = original->SelectSections({ uint32_t(original->GetSections().size() - 1) });
                CheckComponent(first.SetGeometry(subset), "Subset geometry");
                CheckComponentCollision(first, 1.0, 1e-9);
                CheckComponent(first.SetGeometry(original), "Subset restore");
            }

            // 원본 → 생성 → 생성 부분집합 → 원본 순서로 바꿔도 생성 revision은 hull이 없고 원본은 정확히 복원되어야 한다.
            auto buffers = std::make_shared<GeometryBuffers>();
            buffers->Source = original->GetSource();
            const auto generated = GeometryRevision::FromGenerated(buffers);
            for (const auto& revision : { generated, generated->SelectSections({}) })
            {
                CheckComponent(first.SetGeometry(revision), "Generated geometry");
                CheckComponent(first.GetCollisionStatus() == ComponentCollisionStatus::Unavailable
                    && first.GetCollision().ConvexElems.empty()
                    && first.GetCollisionRevisionId() == revision->GetId(), "Generated revision has no hull");
            }
            CheckComponent(first.SetGeometry(original), "Generated restore");
            CheckComponentCollision(first, 1.0, 1e-9);
            CheckCollisionAgainstFrame(first, SceneTransform());

            // 정적·스킨 section이 섞이면 하나의 강체가 될 수 없으므로 전체가 Unavailable이어야 한다(실제 혼합 모델이 없어 가짜 상태 목록으로 검사한다).
            CheckMixedSectionStatus(*original->GetSource());
            CheckComponent(object->IsColliderCurrent(colliderId), "Restored collider revision");
            CheckComponent(object->RemoveColliderComponent(colliderId), "Collider removal");


            // 컴포넌트 제거와 ID 비재사용, 독립 pose 소유권을 확인한다.
            const ComponentId removed = object->AddMeshComponent(original).GetId();
            const ComponentId dependent = object->AddColliderComponent(removed).Id;
            CheckComponent(object->RemoveMeshComponent(removed), "Mesh removal");
            CheckComponent(!object->FindMeshComponent(removed) && !object->IsColliderCurrent(dependent), "Removed references");
            CheckComponent(!object->RemoveMeshComponent(removed), "Repeated removal rejected");
            MeshComponent& second = object->AddMeshComponent(original);
            CheckComponent(second.GetId() > dependent, "Component IDs not reused");
            CheckComponent(object->GetModelInstance() == pose, "One animator per owner");
            CheckComponent(&first.GetPosedGeometry() != &second.GetPosedGeometry(), "Independent posed ownership");


            // 잘못된 TRS와 배치의 원자성을 확인한다.
            SceneTransform invalid;
            invalid.SetScale(Vector3(0.0f, 1.0f, 1.0f));
            CheckComponent(!second.SetTransform(invalid, MeshTransformMode::Attached), "Invalid TRS rejected");
            DeformBinding binding;
            binding.SourceFrame = SceneTransform(Quaternion(Vector3(kYUnitVector), 0.35f),
                Vector3(1.2f, 0.7f, 1.1f), Vector3(0.5f, 0.3f, 0.0f));
            binding.Pose = PoseMode::Frozen;
            CheckComponent(!second.SetPlacement(invalid, MeshTransformMode::Attached, binding), "Placement validation");
            CheckComponent(second.GetTransformMode() == MeshTransformMode::Attached, "Failed placement is atomic");
            CheckComponent(second.SetPlacement(binding.SourceFrame, MeshTransformMode::Attached, binding), "Source placement");


            // 비항등 SourceFrame의 hull은 Model hull × SourceFrame⁻¹이며 부피는 1/(sx·sy·sz)배다.
            const Vector3 sourceScale = binding.SourceFrame.GetScale();
            CheckComponentCollision(second, 1.0 / (double(sourceScale.GetX()) * double(sourceScale.GetY()) * double(sourceScale.GetZ())), 1e-4);
            CheckCollisionAgainstFrame(second, binding.SourceFrame);

            // 같은 revision에서 SourceFrame을 두 번 바꿔도 누적 없이 마지막 값만 반영되어야 한다.
            DeformBinding other = binding;
            other.SourceFrame = SceneTransform(Quaternion(Vector3(kXUnitVector), -0.6f),
                Vector3(0.8f, 1.5f, 0.9f), Vector3(-1.0f, 0.25f, 2.0f));
            CheckComponent(second.SetDeformBinding(other), "Second source frame");
            CheckCollisionAgainstFrame(second, other.SourceFrame);
            CheckComponent(second.SetDeformBinding(binding), "Source frame restore");
            CheckCollisionAgainstFrame(second, binding.SourceFrame);

            // SourceFrame이 월드 배치에서 정확히 한 번만 상쇄되는지 확인한다.
            const Vector3 point(0.3f, 0.7f, -0.2f);
            const Vector3 expected = object->GetTransform().ToAffine() * point;
            const Vector3 actual = second.GetWorldTransform(object->GetTransform())
                * Vector3(Invert(binding.SourceFrame.ToAffine()) * point);
            CheckComponent(float(Length(actual - expected)) < 0.001f, "Source frame cancels once");

            // 기본 장면 owner는 균일 크기이므로 이 합성에는 전단이 없다.
            SceneTransform target(object->GetTransform().GetRotation() * binding.SourceFrame.GetRotation(),
                object->GetTransform().GetScale() * binding.SourceFrame.GetScale(),
                object->GetTransform().ToAffine() * binding.SourceFrame.GetTranslation() + Vector3(0.0f, 4.5f, 0.0f));
            CheckComponent(second.SetPlacement(target, MeshTransformMode::WorldIndependent, binding), "Detached placement");
            SceneTransform changed = object->GetTransform();
            changed.SetTranslation(Vector3(100.0f, 20.0f, -30.0f));
            const Vector3 independent = second.GetWorldTransform(changed) * point;
            const Vector3 unchanged = second.GetWorldTransform(object->GetTransform()) * point;
            CheckComponent(float(Length(independent - unchanged)) < 0.0001f, "Detached ignores owner");

            // 두 컴포넌트에 collider를 붙여 이후 프레임 검사를 준비한다.
            object->AddColliderComponent(firstId);
            object->AddColliderComponent(second.GetId());
            ++checked;
        }
        Utility::Printf("MeshComponent checks: structure passed, owners=%zu\n", checked);
    }

    // 첫 프레임 pose 준비 후 실제 GPU 자원 분리와 collider 대응을 확인한다.
    inline void CheckPreparedMeshComponents(const Scene& scene, GraphicsContext& context)
    {
        for (const auto& object : scene.GetObjects())
        {
            if (!object->GetModelInstance())
                continue;
            const auto& meshes = object->GetMeshComponents();
            CheckComponent(meshes.size() == 2, "Two components per owner");
            for (const auto& mesh : meshes)
            {
                CheckComponent(mesh->HasPoseBounds(), "Prepared pose bounds");
                if (Renderer::UseComputePose)
                    CheckComponent(mesh->GetPosedGeometry().IsReady(), "Compute pose ready");
            }
            CheckComponent(meshes[0]->GetPosedGeometry().GetBuffer().GetGpuVirtualAddress()
                != meshes[1]->GetPosedGeometry().GetBuffer().GetGpuVirtualAddress(), "Distinct GPU buffers");
            ModelInstance invalidPose(nullptr);
            meshes.front()->Update(context, invalidPose, object->GetTransform());
            CheckComponent(!meshes.front()->HasPoseBounds(), "Invalid pose clears query readiness");
            meshes.front()->Update(context, *object->GetModelInstance(), object->GetTransform());
            for (const auto& collider : object->GetColliderComponents())
                CheckComponent(object->IsColliderCurrent(collider.Id), "Live collider target");
        }
        Utility::Printf("MeshComponent checks: GPU preparation passed\n");
    }
}
