#include "PhysicsWorldInternal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>

namespace MiniPhysics
{
    // 여러 조회 world가 같은 Jolt 등록 정보를 공유하도록 초기화한다.
    Runtime::Runtime()
    {
        if (JPH::Factory::sInstance) throw std::runtime_error("Jolt runtime already owned externally");
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }

    // 마지막 world가 사라진 뒤 전역 형식 등록을 해제한다.
    Runtime::~Runtime()
    {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }

    // 기존 world가 살아 있으면 동일 runtime을 재사용한다.
    std::shared_ptr<Runtime> AcquireRuntime()
    {
        static std::mutex mutex;
        static std::weak_ptr<Runtime> weak;
        std::lock_guard<std::mutex> guard(mutex);
        auto runtime = weak.lock();
        if (!runtime) { runtime = std::make_shared<Runtime>(); weak = runtime; }
        return runtime;
    }

    // 경계의 중심 반올림까지 포함하도록 Box 반크기를 보수적으로 만든다.
    JPH::RefConst<JPH::Shape> MakeBox(const QueryBounds& bounds, JPH::RVec3& center)
    {
        float c[3], half[3];
        for (size_t i = 0; i < 3; ++i)
        {
            const double mid = (double(bounds.Minimum[i]) + double(bounds.Maximum[i])) * 0.5;
            c[i] = float(mid);
            const double radius = (double(bounds.Maximum[i]) - double(bounds.Minimum[i])) * 0.5 + std::abs(double(c[i]) - mid);
            half[i] = std::nextafter(std::max(1.0e-4f, float(radius)), std::numeric_limits<float>::infinity());
        }
        center = JPH::RVec3(c[0], c[1], c[2]);
        return new JPH::BoxShape(JPH::Vec3(half[0], half[1], half[2]), 0.0f);
    }

    // body 경계 계산이 오버플로하지 않는 유한 범위를 검사한다.
    bool QueryBounds::IsValid() const
    {
        for (size_t i = 0; i < 3; ++i)
            if (!std::isfinite(Minimum[i]) || !std::isfinite(Maximum[i]) || Minimum[i] > Maximum[i] ||
                std::abs(Minimum[i]) > 1.0e15f || std::abs(Maximum[i]) > 1.0e15f) return false;
        return true;
    }

    // 불완전한 구현 타입의 수명은 이 번역 단위에서 관리한다.
    PhysicsWorld::PhysicsWorld() = default;
    // 명시적 종료를 생략해도 등록 body를 남기지 않는다.
    PhysicsWorld::~PhysicsWorld() { Shutdown(); }

    // 아직 simulation하지 않는 조회 전용 world를 초기화한다.
    void PhysicsWorld::Initialize(uint32_t capacity)
    {
        if (m_Impl) return;
        if (capacity == 0 || capacity > MaxColliderCapacity) throw std::invalid_argument("Invalid collider capacity");

        // Jolt world를 만든다.
        auto impl = std::make_unique<Impl>();
        impl->World.Init(capacity, 0, capacity, capacity, impl->Layers, impl->Layers, impl->Layers);

        // 예약 상한을 기록하고 handle 맵을 용량만큼 미리 잡아 Commit이 재해시하지 않게 한다.
        impl->Capacity = capacity;
        impl->SimBodies.reserve(capacity);

        // 완성된 상태를 공개한다.
        m_Impl = std::move(impl);
    }

    // 남은 준비 body를 고아로 만들고 body를 모두 제거한 뒤 마지막 runtime 참조를 해제한다.
    void PhysicsWorld::Shutdown()
    {
        // 살아 있는 PreparedBody가 해제된 world를 건드리지 않도록 소유 world 연결을 끊는다.
        if (m_Impl) for (auto* data : m_Impl->Prepared) data->Owner = nullptr;

        // 게시된 body를 비우고 world를 닫는다.
        Clear();
        m_Impl.reset();
    }

    // 원본 메시와 별개인 보수적 조회 Box를 생성한다.
    ColliderHandle PhysicsWorld::CreateQueryBox(const QueryBounds& bounds)
    {
        if (!m_Impl || !bounds.IsValid() || m_NextHandle == std::numeric_limits<ColliderHandle>::max()) return InvalidCollider;
        if (m_Impl->Reserved() >= m_Impl->Capacity) return InvalidCollider;
        JPH::RVec3 center;
        const auto shape = MakeBox(bounds, center);
        JPH::BodyCreationSettings settings(shape, center, JPH::Quat::sIdentity(), JPH::EMotionType::Static, ObjectLayers::Query);
        const ColliderHandle handle = m_NextHandle++;
        settings.mUserData = handle;
        auto& bodies = m_Impl->World.GetBodyInterface();
        JPH::Body* body = bodies.CreateBody(settings);
        if (!body) return InvalidCollider;
        const auto id = body->GetID();
        try { m_Impl->Bodies.emplace(handle, id); }
        catch (...) { bodies.DestroyBody(id); throw; }
        bodies.AddBody(id, JPH::EActivation::DontActivate);
        return handle;
    }

    // 유효 handle의 shape와 월드 중심을 함께 갱신한다.
    bool PhysicsWorld::UpdateQueryBox(ColliderHandle handle, const QueryBounds& bounds)
    {
        if (!m_Impl || !bounds.IsValid()) return false;
        const auto entry = m_Impl->Bodies.find(handle);
        if (entry == m_Impl->Bodies.end()) return false;
        JPH::RVec3 center;
        const auto shape = MakeBox(bounds, center);
        auto& bodies = m_Impl->World.GetBodyInterface();
        bodies.SetShape(entry->second, shape, false, JPH::EActivation::DontActivate);
        bodies.SetPosition(entry->second, center, JPH::EActivation::DontActivate);
        return true;
    }

    // body 삭제 후 같은 수치의 오래된 handle을 다시 사용하지 않는다.
    bool PhysicsWorld::RemoveCollider(ColliderHandle handle)
    {
        if (!m_Impl) return false;
        const auto entry = m_Impl->Bodies.find(handle);
        if (entry == m_Impl->Bodies.end()) return false;
        auto& bodies = m_Impl->World.GetBodyInterface();
        bodies.RemoveBody(entry->second);
        bodies.DestroyBody(entry->second);
        m_Impl->Bodies.erase(entry);
        return true;
    }

    // 조회와 simulation body를 모두 해제하며 stale handle을 무효로 유지한다. 준비만 된 body는 건드리지 않는다.
    void PhysicsWorld::Clear()
    {
        if (!m_Impl) return;

        // 조회 body를 모두 제거한다.
        while (!m_Impl->Bodies.empty()) RemoveCollider(m_Impl->Bodies.begin()->first);

        // simulation body를 모두 제거하고 파괴한다.
        while (!m_Impl->SimBodies.empty()) DestroyBody(m_Impl->SimBodies.begin()->first);
    }

    // Query 계층의 body만 대상으로 broadphase와 객체 계층 필터를 함께 적용한다.
    std::vector<ColliderHandle> PhysicsWorld::QueryAabb(const QueryBounds& bounds) const
    {
        std::vector<ColliderHandle> result;
        if (!m_Impl || !bounds.IsValid()) return result;
        JPH::AllHitCollisionCollector<JPH::CollideShapeBodyCollector> collector;
        const JPH::AABox queryBox(
            JPH::Vec3(bounds.Minimum[0], bounds.Minimum[1], bounds.Minimum[2]),
            JPH::Vec3(bounds.Maximum[0], bounds.Maximum[1], bounds.Maximum[2]));
        const JPH::SpecifiedBroadPhaseLayerFilter broadPhaseFilter(BroadPhaseLayers::Query);
        const JPH::SpecifiedObjectLayerFilter objectFilter(ObjectLayers::Query);
        m_Impl->World.GetBroadPhaseQuery().CollideAABox(queryBox, collector, broadPhaseFilter, objectFilter);
        for (const auto& id : collector.mHits)
        {
            const JPH::BodyLockRead body(m_Impl->World.GetBodyLockInterface(),id);
            if (body.Succeeded() && body.GetBody().GetWorldSpaceBounds().Overlaps(queryBox))
                result.push_back(body.GetBody().GetUserData());
        }
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    }

    // 디버그와 수명 검증용 등록 수를 반환한다.
    size_t PhysicsWorld::GetColliderCount() const { return m_Impl ? m_Impl->Bodies.size() : 0; }
}
