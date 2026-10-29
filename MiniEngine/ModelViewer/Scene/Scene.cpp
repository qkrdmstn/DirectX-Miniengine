#include "Scene.h"
#include <algorithm>
#include <cassert>
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
        // 편집 잠금 객체는 기즈모·Undo 모두 같은 경로에서 거절한다.
        if (!IsEditable()) return false;

        // 입력을 정규화한 뒤 owner 배치를 저장한다.
        SceneTransform normalized = transform;
        if (!normalized.Normalize())
            return false;
        m_Transform = normalized;
        return true;
    }

    // 명시적 편집 잠금 여부만 본다. simulate 조각은 Mesh 대상으로 따로 편집하며 owner 편집은 Attached 컴포넌트만 움직인다.
    bool GameObject::IsEditable() const
    {
        // 편집 잠금이 없으면 owner 편집을 허용한다.
        return !m_NotEditable;
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
        // 준비와 commit을 연달아 수행한다.
        return CommitMeshComponent(PrepareMeshComponent(std::move(geometry)));
    }

    // 원본 검증, ID 발급, 컴포넌트 생성, 삽입 자리 예약을 owner 변경 없이 수행한다.
    std::unique_ptr<MeshComponent> GameObject::PrepareMeshComponent(std::shared_ptr<const GeometryRevision> geometry)
    {
        // 원본 pose 소유자와 같은 Model인지 검증한다.
        if (!m_ModelInstance || !geometry || geometry->GetSource() != m_ModelInstance->GetModel())
            throw std::invalid_argument("Component source differs from owner pose");

        // 컴포넌트를 만들고 -collision-log 1이면 충돌 요약을 출력한다.
        auto component = std::make_unique<MeshComponent>(AllocateComponentId(), std::move(geometry));
        component->LogCollision(static_cast<uint32_t>(m_Id));

        // commit이 할당하지 않도록 한 칸을 미리 확보한다.
        m_MeshComponents.reserve(m_MeshComponents.size() + 1);
        return component;
    }

    // 예약된 자리에 준비된 컴포넌트를 넣어 owner에 공개한다.
    MeshComponent& GameObject::CommitMeshComponent(std::unique_ptr<MeshComponent> prepared) noexcept
    {
        // 예약 용량 안에서만 삽입한다.
        assert(m_MeshComponents.size() < m_MeshComponents.capacity());
        m_MeshComponents.push_back(std::move(prepared));
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

    // 지정한 collider를 찾은 뒤 현재 geometry revision과 비교해 오래된 참조를 거부한다.
    bool GameObject::IsColliderCurrent(ComponentId id) const
    {
        // ID가 일치하는 collider에 대해서만 메시 목록을 검색한다.
        for (const ColliderComponent& collider : m_ColliderComponents)
        {
            if (collider.Id != id)
                continue;
            const MeshComponent* mesh = FindMeshComponent(collider.MeshId);
            return mesh && mesh->GetGeometry()->GetId() == collider.GeometryRevisionId;
        }

        // 등록되지 않은 collider는 유효하지 않다.
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

    // GPU 작업 없이 owner당 한 번 애니메이션을 평가하여 물리 입력 pose를 만든다.
    void Scene::EvaluateAnimations(float deltaTime)
    {
        // 모델 없는 객체를 건너뛰고 각 인스턴스의 애니메이션 시간만 진행한다.
        for (const auto& object : m_Objects)
            if (object->m_ModelInstance)
                object->m_ModelInstance->EvaluateAnimation(deltaTime);
    }

    // 모든 모델을 애니메이션 pose로 확정한다.
    // 개별 물리 writeback보다 먼저 호출한다.
    bool Scene::FinalizePoses()
    {
        // 한 owner가 실패해도 다른 owner는 확정하여 독립적인 렌더 유효성을 유지한다.
        bool valid = true;
        for (const auto& object : m_Objects)
            if (object->m_ModelInstance && !object->m_ModelInstance->FinalizePose(nullptr, 0))
                valid = false;
        return valid;
    }

    // ID로 찾은 owner에게만 override를 전달하고 모델 없는 객체나 삭제된 ID는 거절한다.
    bool Scene::FinalizeObjectPose(ObjectId id, const NodePhysicsPose* overrides, uint32_t overrideCount)
    {
        // Scene이 소유한 모델 인스턴스 안에서만 최종 node pose와 팔레트를 변경한다.
        GameObject* object = FindObject(id);
        if (!object || !object->m_ModelInstance)
            return false;
        return object->m_ModelInstance->FinalizePose(overrides, overrideCount);
    }

    // CPU pose를 다시 평가하지 않고 각 컴포넌트의 GPU geometry를 준비한다.
    void Scene::PrepareMeshes(GraphicsContext& context)
    {
        // 무효 pose도 Update에 전달하여 이전 준비 상태를 해제하고 GPU 기록을 생략한다.
        for (const auto& object : m_Objects)
        {
            if (!object->m_ModelInstance)
                continue;
            for (const auto& mesh : object->m_MeshComponents)
                mesh->Update(context, *object->m_ModelInstance, object->m_Transform);
        }
    }

    // 기존 비물리 호출 순서를 유지하며 분리된 CPU·GPU 단계를 연속 수행한다.
    void Scene::Update(GraphicsContext& context, float deltaTime)
    {
        // 애니메이션과 override 없는 최종 pose를 만든 뒤 GPU 준비를 한 번 수행한다.
        EvaluateAnimations(deltaTime);
        FinalizePoses();
        PrepareMeshes(context);
    }

    // 같은 owner의 모든 메시 컴포넌트를 각 렌더 pass에 등록한다.
    void Scene::Render(Renderer::MeshSorter& sorter) const
    {
        for (const auto& object : m_Objects)
            for (const auto& mesh : object->m_MeshComponents)
                mesh->Render(sorter);
    }
}
