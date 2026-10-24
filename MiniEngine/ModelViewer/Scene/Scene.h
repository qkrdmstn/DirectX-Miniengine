#pragma once

#include "Model.h"
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

        // Reject invalid transforms and keep the renderer's locator synchronized.
        bool SetTransform(const SceneTransform& transform);
        void LoopAllAnimations();

    private:
        friend class Scene;
        GameObject(ObjectId id, std::string name, std::shared_ptr<const Model> model);

        ObjectId m_Id;
        std::string m_Name;
        SceneTransform m_Transform{ Math::kIdentity };
        std::unique_ptr<ModelInstance> m_ModelInstance;
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
