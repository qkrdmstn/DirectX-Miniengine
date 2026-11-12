#include "PhysicsWorldInternal.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace MiniPhysics
{
    namespace
    {
        class LineDebugRenderer final : public JPH::DebugRendererSimple
        {
        public:
            // 이번 수집의 출력 벡터를 연결한다. 선 데이터는 호출자가 소유한다.
            void SetOutput(std::vector<DebugLine>* lines)
            {
                m_Lines = lines;
            }

            // Jolt 월드 선분과 원래 색을 중립 자료형으로 수집한다.
            void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override
            {
                // 출력이 연결되지 않은 호출은 버린다.
                if (!m_Lines)
                    return;

                // 바이트 색을 overlay가 받는 정규화 RGBA로 바꾼다.
                m_Lines->push_back({ { from.GetX(), from.GetY(), from.GetZ() },
                    { to.GetX(), to.GetY(), to.GetZ() },
                    { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f } });
            }

            // 삼각형은 채우지 않고 세 모서리만 수집한다.
            void DrawTriangle(JPH::RVec3Arg a, JPH::RVec3Arg b, JPH::RVec3Arg c,
                JPH::ColorArg color, ECastShadow) override
            {
                // 각 모서리를 같은 색의 월드 선분으로 낸다.
                DrawLine(a, b, color);
                DrawLine(b, c, color);
                DrawLine(c, a, color);
            }

            // 이 overlay는 3D 텍스트를 표시하지 않는다.
            void DrawText3D(JPH::RVec3Arg, const JPH::string_view&, JPH::ColorArg, float) override
            {
                // 텍스트 요청은 무시한다.
            }

        private:
            std::vector<DebugLine>* m_Lines = nullptr;
        };
    }

    // Jolt가 보는 body 형상과 관절을 이번 프레임의 선분으로 수집한다.
    void PhysicsWorld::CollectDebugLines(std::vector<DebugLine>& out, bool bodies, bool constraints) const
    {
        // 초기화 전에도 이전 프레임 선분이 남지 않게 한다.
        out.clear();
        if (!m_Impl || (!bodies && !constraints))
            return;

        // 도형 자료를 매 프레임 다시 만들지 않도록 runtime의 렌더러 하나를 재사용한다.
        auto& shared = m_Impl->RuntimeOwner->DebugLines;
        if (!shared)
            shared = std::make_unique<LineDebugRenderer>();
        LineDebugRenderer& renderer = static_cast<LineDebugRenderer&>(*shared);
        renderer.SetOutput(&out);

        // DrawBodies가 내부에서 모든 body를 잠그므로 외부 body 잠금을 중첩하지 않는다.
        if (bodies)
        {
            JPH::BodyManager::DrawSettings settings;
            settings.mDrawShape = true;
            settings.mDrawShapeWireframe = true;
            settings.mDrawBoundingBox = false;
            m_Impl->World.DrawBodies(settings, &renderer);
        }

        // Step과 변경이 끝난 장면 스레드에서 읽으며 Jolt가 관절 목록 잠금을 담당한다.
        if (constraints)
            m_Impl->World.DrawConstraints(&renderer);

        // 수집이 끝나면 호출자 벡터 연결을 끊는다.
        renderer.SetOutput(nullptr);
    }

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
        // 디버그 렌더러의 도형 자료를 Jolt 등록 해제 전에 먼저 놓는다.
        DebugLines.reset();

        // 타입 등록과 Factory를 해제한다.
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

    // 조회와 simulation에 사용할 Jolt world와 물리 설정을 초기화한다.
    void PhysicsWorld::Initialize(uint32_t capacity)
    {
        // 중복 초기화와 잘못된 용량을 검사한다.
        if (m_Impl) return;
        if (capacity == 0 || capacity > MaxColliderCapacity) throw std::invalid_argument("Invalid collider capacity");

        // Jolt world를 만든다.
        auto impl = std::make_unique<Impl>();
        impl->OwnerTable->DisableCollision(0, 1);
        impl->PairFilter->OwnerTable = impl->OwnerTable;
        impl->World.Init(capacity, 0, capacity, capacity, impl->Layers, impl->Layers, impl->Layers);
        impl->World.SetContactListener(&impl->Contacts);

        // R14 관통 한계 0.01 m의 절반을 위치 보정 허용치로 지정한다.
        constexpr float PenetrationSlop = 0.005f;
        JPH::PhysicsSettings settings = impl->World.GetPhysicsSettings();
        settings.mPenetrationSlop = PenetrationSlop;
        impl->World.SetPhysicsSettings(settings);

        // Y-up 중력(Unreal -980 cm/s²를 m로 환산한 값)을 지정한다.
        impl->World.SetGravity(JPH::Vec3(0.0f, -9.8f, 0.0f));

        // Step용 임시 할당기와 작업 스레드 풀을 만든다. 할당기는 10 MB(Jolt 권장 크기)이고, 스레드는 메인 스레드를 제외한 코어 수(최소 1)다.
        const unsigned cores = std::thread::hardware_concurrency();
        const int workers = cores > 1 ? int(cores) - 1 : 1;
        impl->TempAllocator = std::make_unique<JPH::TempAllocatorImpl>(10u * 1024u * 1024u);
        impl->JobSystem = std::make_unique<JPH::JobSystemThreadPool>(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, workers);

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

        // body 제거와 world 파괴 전에 listener 연결을 끊는다.
        if (m_Impl) m_Impl->World.SetContactListener(nullptr);

        // 게시된 body를 비운다.
        Clear();

        // 작업 스레드와 임시 할당기를 먼저 정리한 뒤 world를 닫는다.
        if (m_Impl)
        {
            m_Impl->JobSystem.reset();
            m_Impl->TempAllocator.reset();
        }
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
        // body보다 먼저 관절 참조를 모두 해제한다.
        if (!m_Impl) return;
        while (!m_Impl->Constraints.empty()) RemoveConstraint(m_Impl->Constraints.begin()->first);

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

    // simulation handle을 검증하고 읽기 잠금 아래 실제 sensor 상태를 읽는다.
    bool PhysicsWorld::IsSensor(BodyHandle handle) const
    {
        // 무효 world와 handle은 sensor로 세지 않는다.
        if (!m_Impl) return false;
        const auto found = m_Impl->SimBodies.find(handle);
        if (found == m_Impl->SimBodies.end()) return false;

        // 잠금이 유효한 동안만 Jolt body를 읽는다.
        const JPH::BodyLockRead lock(m_Impl->World.GetBodyLockInterface(), found->second);
        return lock.Succeeded() && lock.GetBody().IsSensor();
    }

    // 작업 스레드에서 누적한 진단 횟수를 원자적으로 읽는다.
    uint32_t PhysicsWorld::SensorContactCount() const
    {
        // 초기화 전이나 종료 후에는 이번 world의 접촉이 없다.
        return m_Impl ? m_Impl->Contacts.Count.load(std::memory_order_relaxed) : 0;
    }

    // 직전 break의 초기 겹침 쌍 수와 누적 분리 해제 수를 진단에 전달한다.
    uint32_t PhysicsWorld::SensorOverlapCount(bool released) const
    {
        // 초기화 전이나 종료 후에는 유효한 계수가 없다.
        if (!m_Impl) return 0;
        return released ? m_Impl->PairFilter->OverlapReleased : m_Impl->PairFilter->OverlapAdded;
    }

    // 디버그와 수명 검증용 등록 수를 반환한다.
    size_t PhysicsWorld::GetColliderCount() const { return m_Impl ? m_Impl->Bodies.size() : 0; }
}
