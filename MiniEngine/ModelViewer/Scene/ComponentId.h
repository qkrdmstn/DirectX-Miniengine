#pragma once
#include <cstdint>

namespace Viewer
{
    // ID는 GameObject 안에서 발급하며 삭제 후 재사용하지 않는다.
    using ComponentId = uint64_t;
    constexpr ComponentId InvalidComponentId = 0;

    // 실제 물리 body는 후속 PhysicsWorld가 소유한다. 여기에는 대응 관계만 둔다.
    struct ColliderComponent
    {
        ComponentId Id = InvalidComponentId;
        ComponentId MeshId = InvalidComponentId;
        uint64_t GeometryRevisionId = 0;
    };
}
