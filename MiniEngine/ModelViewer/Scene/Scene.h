#pragma once

#include "MeshComponent.h"
#include "SceneTransform.h"
#include "PickHit.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace Viewer
{
    // Scene owns objects; an object's address remains stable until it is deleted.
    class GameObject
    {
    public:
        ObjectId GetId() const { return m_Id; }
        const std::string& GetName() const { return m_Name; }
        // 객체의 편집용 TRS를 읽는다.
        const SceneTransform& GetTransform() const { return m_Transform; }
        const ModelInstance* GetModelInstance() const { return m_ModelInstance.get(); }
        // 바닥 등 절단 대상에서 제외할 객체를 표시한다.
        void SetNotCuttable(bool value) { m_NotCuttable = value; }
        // 바닥 등 사용자 편집에서 제외할 객체를 표시한다.
        void SetNotEditable(bool value) { m_NotEditable = value; }
        // 절단 허용 여부를 읽는다. simulation body의 임시 제한은 별도로 검사한다.
        bool IsCuttable() const { return !m_NotCuttable; }
        // 바닥 등 명시적 편집 잠금이 없는지 읽는다. simulate 조각의 편집은 Mesh 대상 경로가 맡는다.
        bool IsEditable() const;

        // owner 안의 MeshComponent를 ID로 검색한다.
        MeshComponent* FindMeshComponent(ComponentId id);
        // owner 안의 MeshComponent를 읽기 전용으로 검색한다.
        const MeshComponent* FindMeshComponent(ComponentId id) const;
        // 같은 pose를 사용하는 독립 메시 컴포넌트를 추가한다.
        MeshComponent& AddMeshComponent(std::shared_ptr<const GeometryRevision> geometry);
        // 컴포넌트를 만들고 ID를 발급하며 삽입 자리를 예약한다. 아직 owner에 보이지 않고 버려도 ID만 소모된다.
        std::unique_ptr<MeshComponent> PrepareMeshComponent(std::shared_ptr<const GeometryRevision> geometry);
        // 준비된 컴포넌트를 예약한 자리에 넣는다. 할당하지 않으며 Prepare 직후 다른 추가가 없어야 한다.
        MeshComponent& CommitMeshComponent(std::unique_ptr<MeshComponent> prepared) noexcept;
        // 대응 collider와 함께 메시를 제거하며 ID는 재사용하지 않는다.
        bool RemoveMeshComponent(ComponentId id);
        // 컴포넌트별 geometry·pose·변환을 읽는다.
        const std::vector<std::unique_ptr<MeshComponent>>& GetMeshComponents() const { return m_MeshComponents; }
        // 현재 geometry revision에 대응하는 collider 참조를 추가한다.
        const ColliderComponent& AddColliderComponent(ComponentId meshId);
        // collider 참조를 ID로 제거한다.
        bool RemoveColliderComponent(ComponentId id);
        // 물리 body가 아닌 collider 대응 설명 목록을 읽는다.
        const std::vector<ColliderComponent>& GetColliderComponents() const { return m_ColliderComponents; }
        // 삭제·geometry 교체로 오래된 collider 대응을 거부한다.
        bool IsColliderCurrent(ComponentId id) const;
        // 현재 geometry에 대응하는 collider의 조회 handle만 갱신한다.
        bool SetColliderQueryHandle(ComponentId id, uint64_t handle);

        // 유효한 owner TRS를 저장한다. WorldIndependent 메시에는 곱하지 않는다.
        bool SetTransform(const SceneTransform& transform);
        // 지정한 애니메이션 하나를 재생하며 기본값으로 반복한다.
        void PlayAnimation(uint32_t animationIndex, bool loop = true);
        void LoopAllAnimations();

    private:
        friend class Scene;
        GameObject(ObjectId id, std::string name, std::shared_ptr<const Model> model);

        // mesh·collider가 공유하는 owner 범위 ID를 고갈 검사 후 발급한다.
        ComponentId AllocateComponentId();

        ObjectId m_Id;
        std::string m_Name;
        bool m_NotCuttable = false, m_NotEditable = false;
        SceneTransform m_Transform{ Math::kIdentity };
        std::unique_ptr<ModelInstance> m_ModelInstance;
        ComponentId m_NextComponentId = 1;
        std::vector<std::unique_ptr<MeshComponent>> m_MeshComponents;
        std::vector<ColliderComponent> m_ColliderComponents;
    };

    class Scene
    {
    public:
        GameObject& CreateObject(std::string name, std::shared_ptr<const Model> model = nullptr);
        GameObject* FindObject(ObjectId id);
        const GameObject* FindObject(ObjectId id) const;
        bool DeleteObject(ObjectId id);
        void Clear();

        const std::vector<std::unique_ptr<GameObject>>& GetObjects() const { return m_Objects; }
        void Update(GraphicsContext& context, float deltaTime);
        void Render(Renderer::MeshSorter& sorter) const;

    private:
        // Clear and deletion never reset this counter, so stale IDs stay invalid.
        ObjectId m_NextId = 1;
        std::vector<std::unique_ptr<GameObject>> m_Objects;
    };
}
