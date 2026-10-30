#pragma once

#include "PickHit.h"
#include "../../Physics/PhysicsWorld.h"
#include "../../Model/SkeletonPose.h"
#include "VectorMath.h"
#include <map>
#include <vector>

namespace Viewer
{
    // 다음 step의 body 원점 목표와 COM 선속도·월드 각속도를 보관한다.
    struct SkeletalBodyTarget
    {
        MiniPhysics::SkeletalRigidFrame Frame;
        std::array<double, 3> Linear{}, Angular{};
        float Scale = 1;
    };

    struct BreakRequest
    {
        ObjectId Owner = InvalidObjectId;
        uint32_t JointId = MiniPhysics::InvalidSkeletalId;
        uint64_t ExpectedRigGeneration = 0, ExpectedTopologyGeneration = 0;
        std::array<float, 3> WorldImpulse{}, WorldPoint{};
    };

    // 렌더 진단은 목표가 아니라 step 이후 실제 body 상태를 읽는다.
    struct SkeletalBodyState
    {
        uint32_t Node = MiniPhysics::InvalidSkeletalId;
        MiniPhysics::SkeletalRigidFrame Frame;
        std::array<double, 3> CenterOfMass{}, Linear{}, Angular{};
        MiniPhysics::BodyMotion Motion = MiniPhysics::BodyMotion::Kinematic;
    };

    struct SkeletalJointState
    {
        bool Broken = false;
        MiniPhysics::ConstraintState Solver;
        Math::Vector3 ParentAnchor{ Math::kZero }, ChildAnchor{ Math::kZero };
    };

    struct SkeletalRigState
    {
        std::vector<SkeletalBodyState> Bodies;
        std::vector<SkeletalJointState> Joints;
    };

    enum class BreakStatus
    {
        Prepared, Applied, NotFound, AlreadyBroken, Stale,
        UnsupportedTopology, UnsupportedFrame, UnsupportedPose, InvalidInput, NotReady
    };

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
        // E와 본 pose에서 배율을 분리하고 Kinematic rig를 등록한다.
        bool RegisterFromPose(ObjectId owner, const MiniPhysics::SkeletalPhysicsAsset& asset,
            const std::vector<uint32_t>& nodeParents, const Math::Matrix4& modelToWorld,
            const std::vector<Math::Matrix4>& nodePoses);
        // 모든 frame·handle을 선검증하고 Kinematic 목표만 갱신한다.
        bool UpdateKinematicTargets(ObjectId owner, const Math::Matrix4& modelToWorld,
            const std::vector<Math::Matrix4>& nodePoses, float fixedDt);
        // 저장한 목표와 COM 속도를 읽으며 실패하면 출력을 보존한다.
        bool GetBodyTarget(ObjectId owner, uint32_t bodyId, SkeletalBodyTarget& target) const;
        // 실제 pose·COM·속도·양쪽 anchor를 읽으며 실패하면 출력 전체를 보존한다.
        bool ReadDiagnosticState(ObjectId owner, SkeletalRigState& output) const;
        // 현재 Dynamic body만 등록 배율을 복원한 model node override로 수집한다.
        // 실패하면 호출자의 override 배열을 보존한다.
        bool CollectPoseOverrides(ObjectId owner, const Math::Matrix4& modelToWorld,
            std::vector<NodePhysicsPose>& overrides) const;
        // fixed substep 경계에서 선택 관절만 제거하고 연결된 자식 body들을 물리로 전환한다.
        BreakStatus BreakConstraint(const BreakRequest& request);
        // 성공한 break마다 증가한 topology 번호를 읽으며 미등록 owner이면 0이다.
        uint64_t GetTopologyGeneration(ObjectId owner) const;
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
        friend class PhysicsSystem;
        struct Rig
        {
            uint64_t Generation = 0;
            uint64_t TopologyGeneration = 1;
            std::vector<MiniPhysics::BodyHandle> Bodies;
            std::vector<MiniPhysics::ConstraintHandle> Joints;
            MiniPhysics::SkeletalPhysicsAsset Asset;
            std::vector<uint32_t> NodeParents;
            std::vector<SkeletalBodyTarget> Targets;
            Math::Matrix4 ModelToWorld{ Math::kIdentity };
            float FrameScale = 1;
        };
        // 관절→body 역순으로 world 자원을 정리한다. body 파괴가 pair 제외도 지운다.
        void ReleaseRig(Rig& rig);
        MiniPhysics::PhysicsWorld& m_World;
        std::map<ObjectId, Rig> m_Rigs;
        uint64_t m_NextGeneration = 1;
    };
}
