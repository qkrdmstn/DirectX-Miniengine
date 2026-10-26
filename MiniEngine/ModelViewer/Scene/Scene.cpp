#include "Scene.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <limits>

namespace Viewer
{
    // pose 소유자 하나와 최초 원본 메시 컴포넌트 하나를 만든다.
    GameObject::GameObject(ObjectId id, std::string name, std::shared_ptr<const Model> model)
        : m_Id(id), m_Name(std::move(name))
    {
        if (model)
        {
            m_ModelInstance = std::make_unique<ModelInstance>(model);
            AddMeshComponent(GeometryRevision::FromModel(std::move(model)));
        }
    }

    // 유효한 owner TRS를 저장하고 컴포넌트가 공통 월드 변환 계산에 사용하게 한다.
    bool GameObject::SetTransform(const SceneTransform& transform)
    {
        SceneTransform normalized = transform;
        if (!normalized.Normalize())
            return false;
        m_Transform = normalized;
        return true;
    }

    // 고갈 후에도 0이나 과거 ID를 재발급하지 않는다.
    ComponentId GameObject::AllocateComponentId()
    {
        if (m_NextComponentId == std::numeric_limits<ComponentId>::max())
            throw std::overflow_error("Component IDs exhausted");
        return m_NextComponentId++;
    }

    // 원본 pose 소유자와 같은 Model을 사용하는 geometry만 등록한다.
    MeshComponent& GameObject::AddMeshComponent(std::shared_ptr<const GeometryRevision> geometry)
    {
        if (!m_ModelInstance || !geometry || geometry->GetSource() != m_ModelInstance->GetModel())
            throw std::invalid_argument("Component source differs from owner pose");
        auto component = std::make_unique<MeshComponent>(AllocateComponentId(), std::move(geometry));
        m_MeshComponents.push_back(std::move(component));

        // -collision-log 1이면 새 컴포넌트의 충돌 요약을 출력한다.
        m_MeshComponents.back()->LogCollision(static_cast<uint32_t>(m_Id));
        return *m_MeshComponents.back();
    }

    // 삭제되지 않은 컴포넌트를 owner 범위 ID로 찾는다.
    const MeshComponent* GameObject::FindMeshComponent(ComponentId id) const
    {
        for (const auto& mesh : m_MeshComponents)
            if (mesh->GetId() == id)
                return mesh.get();
        return nullptr;
    }

    // 변경할 메시를 찾으며 잘못된 ID에는 null을 반환한다.
    MeshComponent* GameObject::FindMeshComponent(ComponentId id)
    {
        return const_cast<MeshComponent*>(static_cast<const GameObject&>(*this).FindMeshComponent(id));
    }

    // 메시 삭제 시 대응 collider 참조도 제거한다.
    bool GameObject::RemoveMeshComponent(ComponentId id)
    {
        const auto mesh = std::find_if(m_MeshComponents.begin(), m_MeshComponents.end(),
            [id](const std::unique_ptr<MeshComponent>& entry) { return entry->GetId() == id; });
        if (mesh == m_MeshComponents.end())
            return false;
        m_ColliderComponents.erase(std::remove_if(m_ColliderComponents.begin(), m_ColliderComponents.end(),
            [id](const ColliderComponent& entry) { return entry.MeshId == id; }), m_ColliderComponents.end());
        m_MeshComponents.erase(mesh);
        return true;
    }

    // collider 등록용으로 현재 revision과 대상 메시를 함께 기록한다.
    const ColliderComponent& GameObject::AddColliderComponent(ComponentId meshId)
    {
        const MeshComponent* mesh = FindMeshComponent(meshId);
        if (!mesh)
            throw std::invalid_argument("Collider target does not exist");
        m_ColliderComponents.push_back({ AllocateComponentId(), meshId, mesh->GetGeometry()->GetId() });
        return m_ColliderComponents.back();
    }

    // 오래된 geometry의 cooking 결과를 적용하지 않도록 현재 revision을 비교한다.
    bool GameObject::IsColliderCurrent(ComponentId id) const
    {
        for (const ColliderComponent& collider : m_ColliderComponents)
        {
            const MeshComponent* mesh = FindMeshComponent(collider.MeshId);
            if (collider.Id == id)
                return mesh && mesh->GetGeometry()->GetId() == collider.GeometryRevisionId;
        }
        return false;
    }

    // 현재 geometry에 속한 collider 참조에 SDK 비노출 handle을 기록한다.
    bool GameObject::SetColliderQueryHandle(ComponentId id, uint64_t handle)
    {
        if (!IsColliderCurrent(id)) return false;
        for (auto& collider : m_ColliderComponents)
            if (collider.Id == id) { collider.QueryHandle = handle; return true; }
        return false;
    }

    // 물리 자원을 소유하지 않는 collider 설명을 제거한다.
    bool GameObject::RemoveColliderComponent(ComponentId id)
    {
        const auto collider = std::find_if(m_ColliderComponents.begin(), m_ColliderComponents.end(),
            [id](const ColliderComponent& entry) { return entry.Id == id; });
        if (collider == m_ColliderComponents.end())
            return false;
        m_ColliderComponents.erase(collider);
        return true;
    }

    // 지정한 클립 하나를 재생해 여러 클립이 같은 pose를 동시에 덮어쓰지 않게 한다.
    void GameObject::PlayAnimation(uint32_t animationIndex, bool loop)
    {
        if (m_ModelInstance)
            m_ModelInstance->PlayAnimation(animationIndex, loop);
    }

    void GameObject::LoopAllAnimations()
    {
        if (m_ModelInstance)
            m_ModelInstance->LoopAllAnimations();
    }

    GameObject& Scene::CreateObject(std::string name, std::shared_ptr<const Model> model)
    {
        if (m_NextId == InvalidObjectId)
            throw std::overflow_error("Scene object IDs exhausted");
        auto object = std::unique_ptr<GameObject>(new GameObject(m_NextId, std::move(name), std::move(model)));
        m_Objects.push_back(std::move(object));
        ++m_NextId;
        return *m_Objects.back();
    }

    GameObject* Scene::FindObject(ObjectId id)
    {
        return const_cast<GameObject*>(static_cast<const Scene&>(*this).FindObject(id));
    }

    const GameObject* Scene::FindObject(ObjectId id) const
    {
        for (const auto& object : m_Objects)
            if (object->GetId() == id)
                return object.get();
        return nullptr;
    }

    bool Scene::DeleteObject(ObjectId id)
    {
        const auto object = std::find_if(m_Objects.begin(), m_Objects.end(),
            [id](const std::unique_ptr<GameObject>& entry) { return entry->GetId() == id; });
        if (object == m_Objects.end())
            return false;
        m_Objects.erase(object);
        return true;
    }

    void Scene::Clear()
    {
        m_Objects.clear();
    }

    // pose를 owner당 한 번 평가한 뒤 컴포넌트별 GPU geometry를 준비한다.
    void Scene::Update(GraphicsContext& context, float deltaTime)
    {
        for (const auto& object : m_Objects)
        {
            if (!object->m_ModelInstance)
                continue;
            object->m_ModelInstance->Update(deltaTime);
            for (const auto& mesh : object->m_MeshComponents)
                mesh->Update(context, *object->m_ModelInstance, object->m_Transform);
        }
    }

    // 같은 owner의 모든 메시 컴포넌트를 각 렌더 pass에 등록한다.
    void Scene::Render(Renderer::MeshSorter& sorter) const
    {
        for (const auto& object : m_Objects)
            for (const auto& mesh : object->m_MeshComponents)
                mesh->Render(sorter);
    }
}
