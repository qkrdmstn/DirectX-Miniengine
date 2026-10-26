#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace MiniPhysics
{
    // 조회 전용 보수적 경계다. 낙하용 shape나 질량 계산에 사용하지 않는다.
    struct QueryBounds
    {
        std::array<float, 3> Minimum{}, Maximum{};
        // 유한한 순서 있는 경계인지 검사한다.
        bool IsValid() const;
    };

    using ColliderHandle = uint64_t;
    constexpr ColliderHandle InvalidCollider = 0;
    constexpr uint32_t MaxColliderCapacity = 65536;

    // Jolt 타입과 body 수명을 감춘다. 변경과 조회는 같은 장면 스레드에서 수행한다.
    class PhysicsWorld
    {
    public:
        // SDK 초기화는 명시적인 Initialize 호출까지 보류한다.
        PhysicsWorld();
        // 남은 조회 body와 SDK 자원을 순서대로 해제한다.
        ~PhysicsWorld();
        PhysicsWorld(const PhysicsWorld&) = delete;
        PhysicsWorld& operator=(const PhysicsWorld&) = delete;
        // 조회용 Jolt world를 만들며 이미 초기화된 경우 그대로 유지한다.
        void Initialize(uint32_t capacity = 4096);
        // 모든 body를 제거하고 world를 닫는다. handle은 재사용하지 않는다.
        void Shutdown();
        // 조회 전용 Box를 등록한다. 용량 부족이나 무효 경계는 0을 반환한다.
        ColliderHandle CreateQueryBox(const QueryBounds& bounds);
        // 같은 handle의 경계를 교체하고 오래된 handle은 거절한다.
        bool UpdateQueryBox(ColliderHandle handle, const QueryBounds& bounds);
        // body를 broadphase에서 제거한 뒤 파괴한다.
        bool RemoveCollider(ColliderHandle handle);
        // 등록된 모든 body를 제거하되 world와 handle 세대는 유지한다.
        void Clear();
        // AABB와 겹칠 수 있는 collider handle을 반환한다. 정밀 교차 판정은 아니다.
        std::vector<ColliderHandle> QueryAabb(const QueryBounds& bounds) const;
        // 실제 등록된 조회 body 수를 읽는다.
        size_t GetColliderCount() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
        ColliderHandle m_NextHandle = 1;
    };
}
