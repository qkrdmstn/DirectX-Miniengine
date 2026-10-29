#pragma once

#include "PickHit.h"
#include "../../Physics/PhysicsWorld.h"
#include <map>
#include <vector>

namespace Viewer
{
    // owner별 skeletal rig의 body·관절·인접 pair 수명을 관리한다.
    // world는 PhysicsSystem이 소유하고 여기서는 참조만 한다.
    class SkeletalPhysicsSystem
    {
    public:
        // 공유 world를 연결하며 소유권은 갖지 않는다.
        explicit SkeletalPhysicsSystem(MiniPhysics::PhysicsWorld& world) : m_World(world) {}
        // 남은 rig를 world보다 먼저 해제한다.
        ~SkeletalPhysicsSystem() { Clear(); }
        SkeletalPhysicsSystem(const SkeletalPhysicsSystem&) = delete;
        SkeletalPhysicsSystem& operator=(const SkeletalPhysicsSystem&) = delete;
        // 자산을 검증하고 body별 초기 월드 원점 frame으로 Kinematic body·관절·인접 pair 제외를 만든다.
        // 어느 단계든 실패하면 이미 만든 자원을 역순으로 정리하고 false를 반환한다.
        bool Register(ObjectId owner, const MiniPhysics::SkeletalPhysicsAsset& asset,
            const std::vector<uint32_t>& nodeParents,
            const std::vector<MiniPhysics::SkeletalRigidFrame>& bodyWorldFrames);
        // 관절을 먼저 제거하고 body를 파괴해 pair 제외까지 정리한다.
        // 미등록 owner는 false다.
        bool Unregister(ObjectId owner);
        // 모든 rig를 해제한다.
        // world Shutdown 전에 호출한다.
        void Clear();
        // owner에 등록된 rig가 있는지 읽는다.
        bool IsRegistered(ObjectId owner) const { return m_Rigs.count(owner) != 0; }
        // 등록된 rig 수를 읽는다.
        size_t GetRigCount() const { return m_Rigs.size(); }
        // 등록마다 새로 발급한 번호를 읽으며 미등록이면 0이다.
        uint64_t GetRigGeneration(ObjectId owner) const;
        // 자산 body ID의 runtime handle을 읽으며 없으면 InvalidBody다.
        MiniPhysics::BodyHandle GetBodyHandle(ObjectId owner, uint32_t bodyId) const;
        // 자산 joint ID의 runtime handle을 읽으며 없으면 InvalidConstraint다.
        MiniPhysics::ConstraintHandle GetConstraintHandle(ObjectId owner, uint32_t jointId) const;
        // 진단용 실패 주입점이다.
        // n번째 자원 생성 단계 직전에 실패하며 0이면 꺼짐이다.
        static uint32_t DiagFailRegistrationStep;

    private:
        struct Rig
        {
            uint64_t Generation = 0;
            std::vector<MiniPhysics::BodyHandle> Bodies;
            std::vector<MiniPhysics::ConstraintHandle> Joints;
        };
        // 관절→body 역순으로 world 자원을 정리한다. body 파괴가 pair 제외도 지운다.
        void ReleaseRig(Rig& rig);
        MiniPhysics::PhysicsWorld& m_World;
        std::map<ObjectId, Rig> m_Rigs;
        uint64_t m_NextGeneration = 1;
    };
}
