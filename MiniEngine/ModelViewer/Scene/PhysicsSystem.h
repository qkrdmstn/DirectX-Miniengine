#pragma once

#include "Scene.h"
#include "../../Physics/PhysicsWorld.h"
#include "../../MeshCutting/Geometry/CutPlane.h"
#include <map>

namespace Viewer
{
    // 절단 대상의 불변 입력을 보관한다. Jolt body 포인터는 전달하지 않는다.
    struct CutCandidate
    {
        ObjectId Owner;
        ComponentId Mesh;
        std::shared_ptr<const GeometryRevision> Geometry;
        uint64_t EditGeneration;
        uint64_t PhysicsPoseGeneration;   // 6단계에서 simulate 대상 재확인에 쓴다. 지금은 비교하지 않는다.
        Math::Matrix4 World;
    };

    // Scene의 collider 참조를 Jolt 조회 body에 연결하고 절단 후보 API를 제공한다.
    class PhysicsSystem
    {
    public:
        // 조회를 위한 장면 소유자를 연결한다.
        explicit PhysicsSystem(Scene& scene) : m_Scene(scene) {}
        // collider 참조를 Scene보다 먼저 정리한다.
        ~PhysicsSystem();
        // SDK 조회 사용 여부와 진단용 body 예산을 정한다.
        void Initialize(bool useJolt = true, uint32_t capacity = 4096);
        // 준비된 Static 메시 경계를 등록·갱신하고 삭제된 body를 정리한다.
        void SyncColliders();
        // 최신 SyncColliders 뒤 유한 평면 후보를 반환한다. 실제 split은 GPU가 결정한다.
        std::vector<CutCandidate> QueryCutCandidates(const MeshCutting::CutPlaneData& plane,
            float worldEpsilon = 1.0e-5f) const;
        // 남은 Scene 참조와 SDK world를 정리한다.
        void Clear();
        // 실제 Jolt body 수를 읽는다.
        size_t GetColliderCount() const { return m_World.GetColliderCount(); }
        // SDK 미사용·용량 초과 등으로 기존 경계 검사를 사용하는 수를 읽는다.
        size_t GetFallbackCount() const;
        // 진단 모드에서 기존 전수 검사와 결과가 같은지 대조한다.
        bool ValidateQueries = false;

    private:
        using Key = std::pair<ObjectId, ComponentId>;
        struct Entry
        {
            CutCandidate Candidate{};
            MiniPhysics::QueryBounds Bounds;
            Math::BoundingSphere Sphere{Math::kZero};
            MiniPhysics::ColliderHandle Handle = MiniPhysics::InvalidCollider;
            ComponentId Collider = InvalidComponentId;
            bool Seen = false;
        };
        // 메시 준비 여부와 Static section 조건을 공통으로 검사한다.
        static bool IsEligible(const MeshComponent& mesh);
        // 기존 경계 구와 유한 사각형 AABB 검사를 유지한다.
        static bool Intersects(const Entry& entry, const MeshCutting::CutPlaneData& plane,
            const MiniPhysics::QueryBounds& rectangle, float epsilon);
        // ID·revision·편집·월드 배치가 현재 장면과 일치하는지 검사한다.
        bool IsCurrent(const Entry& entry) const;
        // 삭제·교체 시 참조와 SDK body를 함께 정리한다.
        void RemoveEntry(Entry& entry);
        Scene& m_Scene;
        MiniPhysics::PhysicsWorld m_World;
        std::map<Key, Entry> m_Entries;
        std::map<MiniPhysics::ColliderHandle, Key> m_Handles;
        std::vector<Key> m_FallbackKeys;
        bool m_UseJolt = true;
    };
}
