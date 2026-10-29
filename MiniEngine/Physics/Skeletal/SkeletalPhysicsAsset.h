#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace MiniPhysics
{
    constexpr uint32_t InvalidSkeletalId = UINT32_MAX;

    // 열벡터 강체 frame이며 위치는 m, 회전은 정규화된 xyzw다.
    struct SkeletalRigidFrame
    {
        std::array<double, 3> Position{};
        std::array<double, 4> Rotation{ 0, 0, 0, 1 };
    };

    struct BodyDefinition
    {
        uint32_t Node = InvalidSkeletalId;
        // B = R(E * Gnode) * BoneFromBody: body 좌표를 bone 좌표로 보낸다.
        SkeletalRigidFrame BoneFromBody;
        std::array<double, 3> HalfExtentMeters{};
        double MassKg = 0;
    };

    struct JointDefinition
    {
        uint32_t ParentBody = InvalidSkeletalId;
        uint32_t ChildBody = InvalidSkeletalId;
        // joint 좌표를 각 body 원점 좌표로 보내며 COM 보정은 world 계층에서 한다.
        SkeletalRigidFrame ParentBodyFromJoint;
        SkeletalRigidFrame ChildBodyFromJoint;
        // joint +X가 twist 축이며 +Y/+Z가 두 swing 축이고 단위는 rad다.
        std::array<double, 2> SwingHalfAngles{};
        double TwistMin = 0;
        double TwistMax = 0;
    };

    // ID는 각 vector의 인덱스이며 Model/Jolt 자원이나 런타임 handle을 소유하지 않는다.
    struct SkeletalPhysicsAsset
    {
        std::vector<BodyDefinition> Bodies;
        std::vector<JointDefinition> Joints;
    };

    // NodeId별 부모 배열과 대조하여 단일 body tree와 수치 입력을 검증한다.
    // body 없는 중간 본을 허용하며 자산과 부모 배열을 변경하지 않는다.
    bool ValidateSkeletalPhysicsAsset(const SkeletalPhysicsAsset& asset,
        const std::vector<uint32_t>& nodeParents);
}
