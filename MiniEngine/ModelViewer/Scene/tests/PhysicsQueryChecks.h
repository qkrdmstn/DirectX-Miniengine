#pragma once
#include "../PhysicsSystem.h"
#include "CommandContext.h"
#include "Utility.h"
#include <stdexcept>

namespace Viewer
{
    // Release에서도 장면 통합 검증의 실패를 즉시 보고한다.
    inline void CheckPhysicsQuery(bool condition, const char* message)
    {
        if (!condition) throw std::runtime_error(message);
    }

    // Scene의 실제 pose·geometry·collider 수명과 후보 조회를 GPU 준비 후 검사한다.
    inline void RunPhysicsQueryChecks(Scene& scene, PhysicsSystem& physics, bool useJolt, uint32_t capacity)
    {
        using namespace Math;
        const bool previousValidate = physics.ValidateQueries;
        // 검증 중에는 각 변경의 GPU 준비를 완료하고 다음 장면 변경을 수행한다.
        const auto prepare = [&]()
        {
            auto& context = GraphicsContext::Begin(L"Physics query checks");
            scene.Update(context,0.0f);
            context.Finish(true);
            physics.SyncColliders();
        };
        prepare();
        const size_t baseline = physics.GetColliderCount()+physics.GetFallbackCount();
        std::shared_ptr<const GeometryRevision> geometry;
        for (const auto& object : scene.GetObjects())
            for (const auto& mesh : object->GetMeshComponents())
            {
                if (!mesh->HasPoseBounds()) continue;
                bool skinned = false;
                for (size_t i = 0; i < mesh->GetGeometry()->GetSections().size(); ++i)
                    skinned |= (mesh->GetGeometry()->GetMesh(i).psoFlags & PSOFlags::kHasSkin) != 0;
                if (!skinned) { geometry = mesh->GetGeometry(); break; }
            }
        CheckPhysicsQuery(bool(geometry),"Query self-test requires a static model");
        auto& object = scene.CreateObject("Physics query probe",geometry->GetSource());
        const ObjectId owner = object.GetId();
        auto& first = *object.GetMeshComponents().front();
        const auto original = first.GetGeometry();
        const ComponentId firstId = first.GetId();
        SceneTransform placement;
        placement.SetTranslation(Vector3(10000,10000,10000));
        object.SetTransform(placement);
        auto& second = object.AddMeshComponent(original);
        const ComponentId secondId = second.GetId();
        SceneTransform independent;
        independent.SetTranslation(Vector3(-10000,-10000,-10000));
        second.SetTransform(independent,MeshTransformMode::WorldIndependent);
        prepare();
        CheckPhysicsQuery(physics.GetColliderCount()+physics.GetFallbackCount() == baseline+2,"Two tracked components per owner");
        // 각 컴포넌트 경계 중심의 유한 평면으로 개별 후보를 확인한다.
        const auto planeAt = [&](const MeshComponent& mesh)
        {
            const auto bound = mesh.GetBoundingSphere(object.GetTransform());
            MeshCutting::CutPlaneData plane;
            const float extent = (bound.GetRadius()+1.0f)*4.0f;
            CheckPhysicsQuery(MeshCutting::TryBuildCutPlaneData({extent,extent},
                AffineTransform(Matrix3(kIdentity),bound.GetCenter()),plane),"Probe plane");
            return plane;
        };
        // 같은 Actor 소유여도 다른 MeshId가 하나로 합쳐지지 않는지 검사한다.
        const auto contains = [&](const std::vector<CutCandidate>& hits, ComponentId id)
        {
            for (const auto& hit : hits) if (hit.Owner == owner && hit.Mesh == id) return true;
            return false;
        };
        const auto firstPlane = planeAt(first), secondPlane = planeAt(second);
        auto hits = physics.QueryCutCandidates(firstPlane);
        CheckPhysicsQuery(contains(hits,firstId) && !contains(hits,secondId),"Independent candidate identity");
        auto edited = first.GetTransform();
        edited.SetTranslation(Vector3(4000,0,0));
        first.SetTransform(edited,MeshTransformMode::Attached);
        CheckPhysicsQuery(!contains(physics.QueryCutCandidates(firstPlane),firstId),"Stale pose rejected before sync");
        prepare();
        CheckPhysicsQuery(!contains(physics.QueryCutCandidates(firstPlane),firstId),"Moved collider old area");
        CheckPhysicsQuery(contains(physics.QueryCutCandidates(planeAt(first)),firstId),"Moved collider new area");
        placement.SetRotation(Quaternion(Vector3(kZUnitVector),0.43f));
        placement.SetScale(Vector3(1.4f,0.7f,1.2f));
        object.SetTransform(placement);
        prepare();
        CheckPhysicsQuery(contains(physics.QueryCutCandidates(planeAt(first)),firstId),"Rotated nonuniform owner");
        CheckPhysicsQuery(contains(physics.QueryCutCandidates(secondPlane),secondId),"World independent collider");
        first.SetGeometry(original->SelectSections({}));
        prepare();
        CheckPhysicsQuery(physics.GetColliderCount()+physics.GetFallbackCount() == baseline+1,"Empty revision unregisters collider");
        first.SetGeometry(original);
        prepare();
        CheckPhysicsQuery(physics.GetColliderCount()+physics.GetFallbackCount() == baseline+2,"Revision restore registers collider");
        CheckPhysicsQuery(object.RemoveMeshComponent(secondId),"Remove query mesh");
        prepare();
        CheckPhysicsQuery(!contains(physics.QueryCutCandidates(secondPlane),secondId),"Deleted mesh absent");
        CheckPhysicsQuery(scene.DeleteObject(owner),"Remove query owner");
        prepare();
        CheckPhysicsQuery(physics.GetColliderCount()+physics.GetFallbackCount() == baseline,"Deleted owner pruned");
        physics.Clear();
        CheckPhysicsQuery(physics.GetColliderCount() == 0 && physics.GetFallbackCount() == 0,"Scene query clear");
        physics.Initialize(useJolt,capacity);
        physics.ValidateQueries = true;
        prepare();
        CheckPhysicsQuery(physics.GetColliderCount()+physics.GetFallbackCount() == baseline,"Query reinitialize");
        physics.ValidateQueries = previousValidate;
        Utility::Printf("Physics scene query checks passed: tracked=%zu registered=%zu fallback=%zu\n",
            baseline,physics.GetColliderCount(),physics.GetFallbackCount());
    }
}
