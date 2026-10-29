#include "PhysicsSystem.h"
#include "Utility.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace Viewer
{
    // 초기 충돌 경계를 덮는 최소 1 km 바닥을 첫 step 전에 렌더와 Static Box로 함께 만든다.
    bool PhysicsSystem::CreateFloor(std::shared_ptr<const Model> model)
    {
        // 비활성 world와 중복 생성을 거절하고 삭제된 바닥의 handle은 회수한다.
        if (!m_SimulationEnabled || !model) return false;
        auto* previous = m_Scene.FindObject(m_FloorOwner);
        if (m_FloorBody && previous && previous->FindMeshComponent(m_FloorMesh)) return true;
        if (previous) m_Scene.DeleteObject(m_FloorOwner);
        m_World.DestroyBody(m_FloorBody);
        m_FloorBody = MiniPhysics::InvalidBody;
        m_FloorOwner = InvalidObjectId;

        // GPU pose가 준비되기 전 CPU 충돌 정점에서 초기 월드 경계를 모은다.
        using namespace Math;
        AxisAlignedBox bounds;
        float minimumHeight = std::numeric_limits<float>::max();
        bool hasBounds = false;
        for (const auto& object : m_Scene.GetObjects())
            for (const auto& mesh : object->GetMeshComponents())
            {
                AxisAlignedBox localBounds;
                bool hasMesh = false;
                const AffineTransform world = mesh->GetWorldTransform(object->GetTransform());
                for (const auto& elem : mesh->GetCollision().ConvexElems)
                    for (const auto& vertex : elem.VertexData)
                    {
                        localBounds.AddPoint(world * Vector3(float(vertex.x), float(vertex.y), float(vertex.z)));
                        hasMesh = true;
                    }
                if (!hasMesh) continue;
                const float height = localBounds.GetDimensions().GetY();
                if (!std::isfinite(height) || height < 0.01f)
                {
                    Utility::Printf("Physics floor rejected: collision height must be at least 0.01 m\n");
                    return false;
                }
                bounds.AddBoundingBox(localBounds);
                minimumHeight = std::min(minimumHeight, height);
                hasBounds = true;
            }
        if (!hasBounds) return false;

        // 가장 낮은 물체도 정지 후 y=0 평면을 지나도록 높이를 정하고 초기 낙하 여유를 확보한다.
        const float top = -0.5f * minimumHeight;
        const float lift = std::max(0.0f, top + 0.5f - float(bounds.GetMin().GetY()));

        // 조각이 멀리 이동해도 받칠 수 있게 원점 주위 최소 1 km와 장면 경계 여유를 함께 확보한다.
        constexpr float minimumHalfExtent = 500.0f;
        constexpr float sceneMargin = 2.0f;
        const float minX = std::min(-minimumHalfExtent, float(bounds.GetMin().GetX()) - sceneMargin);
        const float maxX = std::max(minimumHalfExtent, float(bounds.GetMax().GetX()) + sceneMargin);
        const float minZ = std::min(-minimumHalfExtent, float(bounds.GetMin().GetZ()) - sceneMargin);
        const float maxZ = std::max(minimumHalfExtent, float(bounds.GetMax().GetZ()) + sceneMargin);
        const Vector3 dimensions(maxX - minX, 0.5f, maxZ - minZ);
        const Vector3 center((minX + maxX) * 0.5f, top - 0.25f, (minZ + maxZ) * 0.5f);
        const Vector3 cubeSize = model->m_BoundingBox.GetDimensions();
        SceneTransform floorTransform(Quaternion(kIdentity), dimensions / cubeSize,
            center - model->m_BoundingBox.GetCenter() * (dimensions / cubeSize));
        if (!floorTransform.Normalize()) return false;

        // 시작 배치 변경을 미리 검증하고 저장해 게시 실패 시 역순 복원할 수 있게 한다.
        struct Placement
        {
            GameObject* Owner;
            MeshComponent* Mesh;
            SceneTransform Before, After;
        };
        std::vector<Placement> placements;
        for (const auto& object : m_Scene.GetObjects())
        {
            bool moveOwner = false;
            for (const auto& mesh : object->GetMeshComponents())
            {
                if (mesh->GetCollision().ConvexElems.empty()) continue;
                if (mesh->GetTransformMode() == MeshTransformMode::Attached)
                {
                    moveOwner = true;
                    continue;
                }
                SceneTransform next = mesh->GetTransform();
                next.SetTranslation(next.GetTranslation() + Vector3(0, lift, 0));
                if (!next.Normalize()) return false;
                placements.push_back({ object.get(), mesh.get(), mesh->GetTransform(), next });
            }
            if (!moveOwner) continue;
            SceneTransform next = object->GetTransform();
            next.SetTranslation(next.GetTranslation() + Vector3(0, lift, 0));
            if (!next.Normalize()) return false;
            placements.push_back({ object.get(), nullptr, object->GetTransform(), next });
        }

        // 월드 등록 전에 Static Box cook과 용량 예약을 완료한다.
        MiniPhysics::BodyDesc desc;
        desc.Motion = MiniPhysics::BodyMotion::Static;
        desc.StaticBoxHalfExtent = { float(dimensions.GetX()) * 0.5, 0.25, float(dimensions.GetZ()) * 0.5 };
        desc.Position = { center.GetX(), center.GetY(), center.GetZ() };
        MiniPhysics::PreparedBody prepared;
        std::string error;
        if (!m_World.PrepareBody(desc, prepared, &error))
        {
            Utility::Printf("Physics floor rejected: %s\n", error.c_str());
            return false;
        }

        // 바닥 렌더 객체와 검증된 초기 배치를 게시하고 실패하면 모두 복원한다.
        ObjectId created = InvalidObjectId;
        size_t applied = 0;
        try
        {
            auto& object = m_Scene.CreateObject("Physics floor", std::move(model));
            created = object.GetId();
            if (!object.SetTransform(floorTransform)) throw std::runtime_error("Invalid floor transform");
            object.SetNotCuttable(true);
            object.SetNotEditable(true);
            for (auto& placement : placements)
            {
                const bool ok = placement.Mesh ? placement.Mesh->SetTransform(placement.After, MeshTransformMode::WorldIndependent)
                    : placement.Owner->SetTransform(placement.After);
                if (!ok) throw std::runtime_error("Unable to lift initial physics object");
                ++applied;
            }
            const auto handle = m_World.CommitBody(std::move(prepared), false);
            if (!handle) throw std::runtime_error("Unable to commit floor body");
            m_FloorBody = handle;
            m_FloorCenter = desc.Position;
            m_FloorHalf = desc.StaticBoxHalfExtent;
            m_FloorOwner = created;
            m_FloorMesh = object.GetMeshComponents().front()->GetId();
            Utility::Printf("Physics floor: owner=%llu body=%llu top=%.6f size=(%.3f,0.500,%.3f) initialLift=%.6f\n",
                m_FloorOwner, m_FloorBody, top, float(dimensions.GetX()), float(dimensions.GetZ()), lift);
            return true;
        }
        catch (const std::exception& error)
        {
            // 배치와 렌더 객체만 역순 회수하며 미게시 body는 소유 객체가 자동 정리한다.
            while (applied)
            {
                auto& placement = placements[--applied];
                if (placement.Mesh) placement.Mesh->SetTransform(placement.Before, MeshTransformMode::WorldIndependent);
                else placement.Owner->SetTransform(placement.Before);
            }
            if (created) m_Scene.DeleteObject(created);
            Utility::Printf("Physics floor rejected: %s\n", error.what());
            return false;
        }
    }
}
