#include "Scene.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace Viewer
{
    GameObject::GameObject(ObjectId id, std::string name, std::shared_ptr<const Model> model)
        : m_Id(id), m_Name(std::move(name))
    {
        if (model)
            m_ModelInstance = std::make_unique<ModelInstance>(std::move(model));
    }

    bool GameObject::SetTransform(const Math::UniformTransform& transform)
    {
        using namespace DirectX;
        const float scale = transform.GetScale();
        const XMVECTOR position = transform.GetTranslation();
        const XMVECTOR rotation = transform.GetRotation();
        const float rotationLength = XMVectorGetX(XMQuaternionLengthSq(rotation));
        if (!std::isfinite(scale) || scale <= 0.0f ||
            XMVector3IsNaN(position) || XMVector3IsInfinite(position) ||
            XMVector4IsNaN(rotation) || XMVector4IsInfinite(rotation) ||
            !std::isfinite(rotationLength) || rotationLength < 1.0e-12f)
            return false;

        m_Transform = Math::UniformTransform(Math::Quaternion(XMQuaternionNormalize(rotation)),
            scale, transform.GetTranslation());
        if (m_ModelInstance)
            m_ModelInstance->SetTransform(m_Transform);
        return true;
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

    void Scene::Update(GraphicsContext& context, float deltaTime)
    {
        for (const auto& object : m_Objects)
            if (object->m_ModelInstance)
                object->m_ModelInstance->Update(context, deltaTime);
    }

    void Scene::Render(Renderer::MeshSorter& sorter) const
    {
        for (const auto& object : m_Objects)
            if (object->m_ModelInstance)
                object->m_ModelInstance->Render(sorter);
    }
}
