#pragma once

#include "../Scene.h"
#include "Renderer.h"
#include "Utility.h"
#include <stdexcept>

namespace Viewer
{
    // Release에서도 실패를 숨기지 않는 GPU 통합 진단의 조건을 검사한다.
    inline void CheckComponent(bool condition, const char* message)
    {
        if (!condition)
            throw std::runtime_error(message);
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
            const ModelInstance* pose = object->GetModelInstance();
            MeshComponent& first = *object->GetMeshComponents().front();
            const auto original = first.GetGeometry();
            const ComponentId firstId = first.GetId();
            CheckComponent(original->GetSource() == pose->GetModel(), "Source ownership");
            const ComponentId colliderId = object->AddColliderComponent(firstId).Id;
            CheckComponent(object->IsColliderCurrent(colliderId), "Initial collider revision");
            const auto empty = original->SelectSections({});
            CheckComponent(empty->GetId() != original->GetId(), "Revision identity");
            CheckComponent(first.SetGeometry(empty), "Geometry replacement");
            CheckComponent(!object->IsColliderCurrent(colliderId), "Stale collider rejected");
            CheckComponent(first.SetGeometry(original), "Geometry snapshot restore");
            CheckComponent(object->IsColliderCurrent(colliderId), "Restored collider revision");
            CheckComponent(object->RemoveColliderComponent(colliderId), "Collider removal");

            const ComponentId removed = object->AddMeshComponent(original).GetId();
            const ComponentId dependent = object->AddColliderComponent(removed).Id;
            CheckComponent(object->RemoveMeshComponent(removed), "Mesh removal");
            CheckComponent(!object->FindMeshComponent(removed) && !object->IsColliderCurrent(dependent), "Removed references");
            CheckComponent(!object->RemoveMeshComponent(removed), "Repeated removal rejected");
            MeshComponent& second = object->AddMeshComponent(original);
            CheckComponent(second.GetId() > dependent, "Component IDs not reused");
            CheckComponent(object->GetModelInstance() == pose, "One animator per owner");
            CheckComponent(&first.GetPosedGeometry() != &second.GetPosedGeometry(), "Independent posed ownership");

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
