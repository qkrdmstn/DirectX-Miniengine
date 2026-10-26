#pragma once
#include <cstdint>

namespace Viewer
{
    // ID는 GameObject 안에서 발급하며 삭제 후 재사용하지 않는다.
    using ComponentId = uint64_t;
    constexpr ComponentId InvalidComponentId = 0;

    // 조회 body는 PhysicsWorld가 소유하며 여기에는 메시·revision·handle 대응만 둔다.
    struct ColliderComponent
    {
        ComponentId Id = InvalidComponentId;
        ComponentId MeshId = InvalidComponentId;
        uint64_t GeometryRevisionId = 0;
        // SDK 포인터가 아닌 조회 전용 body handle이다. 0이면 경계 검사 fallback이다.
        uint64_t QueryHandle = 0;
    };
}
