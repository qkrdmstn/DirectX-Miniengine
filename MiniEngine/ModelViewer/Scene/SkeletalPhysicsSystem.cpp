#include "SkeletalPhysicsSystem.h"
#include "../../Physics/Collision/AggregateGeom.h"
#include "../../Physics/Collision/BodyMass.h"
#include <cmath>
#include <exception>

namespace Viewer
{
    uint32_t SkeletalPhysicsSystem::DiagFailRegistrationStep = 0;

    namespace
    {
        // body 원점이 중심인 상자를 만들고 공통 Unreal 질량식이 MassKg가 되도록 배율을 구한다.
        bool BuildBoxBody(const MiniPhysics::BodyDefinition& body, MiniCollision::AggregateGeom& geometry, double& massScale)
        {
            // 반크기의 여덟 꼭짓점으로 볼록 상자를 cook한다.
            std::vector<MiniCollision::Vec3d> corners;
            const auto& half = body.HalfExtentMeters;
            for (int i = 0; i < 8; ++i)
                corners.push_back({ (i & 1) ? half[0] : -half[0], (i & 2) ? half[1] : -half[1], (i & 4) ? half[2] : -half[2] });
            if (!MiniCollision::GenerateKDop18(corners, geometry) || geometry.ConvexElems.size() != 1) return false;

            // 배율 1의 질량에 대한 비율이며 최소 질량 하한에 걸리는 상자는 거절한다.
            const double baseMass = MiniCollision::CalculateUnrealMass(geometry.ConvexElems[0].GetVolume());
            if (!(std::isfinite(baseMass) && baseMass > MiniCollision::MinMassKg)) return false;
            massScale = body.MassKg / baseMass;
            return std::isfinite(massScale) && massScale > 0.0;
        }
    }

    // 자산을 검증하고 Kinematic body·관절·인접 pair 제외를 만들며 실패하면 역순으로 되돌린다.
    bool SkeletalPhysicsSystem::Register(ObjectId owner, const MiniPhysics::SkeletalPhysicsAsset& asset,
        const std::vector<uint32_t>& nodeParents,
        const std::vector<MiniPhysics::SkeletalRigidFrame>& bodyWorldFrames)
    {
        // owner·중복 등록·자산 구조·frame 수를 먼저 검사한다.
        if (owner == InvalidObjectId || IsRegistered(owner)) return false;
        if (!MiniPhysics::ValidateSkeletalPhysicsAsset(asset, nodeParents)) return false;
        if (bodyWorldFrames.size() != asset.Bodies.size()) return false;

        // world에 올리기 전 모든 body를 준비하므로 여기서 실패하면 RAII가 예약을 반환한다.
        std::vector<MiniPhysics::PreparedBody> prepared(asset.Bodies.size());
        for (size_t i = 0; i < asset.Bodies.size(); ++i)
        {
            MiniCollision::AggregateGeom geometry;
            MiniPhysics::BodyDesc desc;
            if (!BuildBoxBody(asset.Bodies[i], geometry, desc.MassScale)) return false;
            desc.Geometry = &geometry;
            desc.Position = bodyWorldFrames[i].Position;
            desc.Rotation = bodyWorldFrames[i].Rotation;
            desc.Motion = MiniPhysics::BodyMotion::Kinematic;
            if (!m_World.PrepareBody(desc, prepared[i])) return false;
        }

        // 실패 정리에 필요한 rig 항목과 handle 저장 공간을 자원 생성 전에 확보한다.
        Rig rig;
        auto inserted = m_Rigs.end();
        try
        {
            rig.Bodies.reserve(asset.Bodies.size());
            rig.Joints.reserve(asset.Joints.size());
            inserted = m_Rigs.emplace(owner, Rig{}).first;
        }
        catch (const std::exception&)
        {
            return false;
        }

        // 생성 단계마다 진단 주입 실패를 검사하고 실패하면 만든 자원을 역순으로 정리한다.
        uint32_t step = 0;
        auto fail = [&]()
        {
            ReleaseRig(rig);
            m_Rigs.erase(inserted);
            return false;
        };
        auto allowed = [&]() { return ++step != DiagFailRegistrationStep; };

        // 준비한 body를 자산 ID 순서로 world에 올린다.
        for (auto& body : prepared)
        {
            if (!allowed()) return fail();
            const MiniPhysics::BodyHandle handle = m_World.CommitBody(std::move(body), false);
            if (handle == MiniPhysics::InvalidBody) return fail();
            rig.Bodies.push_back(handle);
        }

        // 두 Kinematic endpoint 관절은 disabled로 만들고 직접 인접 pair의 충돌을 제외한다.
        for (const auto& joint : asset.Joints)
        {
            if (!allowed()) return fail();
            const MiniPhysics::SwingTwistDesc desc{ rig.Bodies[joint.ParentBody], rig.Bodies[joint.ChildBody], joint };
            const MiniPhysics::ConstraintHandle handle = m_World.CreateSwingTwist(desc);
            if (handle == MiniPhysics::InvalidConstraint) return fail();
            rig.Joints.push_back(handle);
            if (!allowed() || !m_World.ExcludeBodyPair(desc.Parent, desc.Child)) return fail();
        }

        // 모든 자원이 만들어진 뒤에만 새 세대와 함께 게시한다.
        rig.Generation = m_NextGeneration++;
        inserted->second = std::move(rig);
        return true;
    }

    // 등록된 owner의 관절과 body를 역순으로 정리한 뒤 항목을 지운다.
    bool SkeletalPhysicsSystem::Unregister(ObjectId owner)
    {
        // 미등록 owner는 아무것도 바꾸지 않는다.
        const auto found = m_Rigs.find(owner);
        if (found == m_Rigs.end()) return false;

        // world 자원을 먼저 해제하고 rig 항목을 제거한다.
        ReleaseRig(found->second);
        m_Rigs.erase(found);
        return true;
    }

    // 모든 rig의 world 자원을 해제한다.
    void SkeletalPhysicsSystem::Clear()
    {
        // 각 rig의 world 자원을 역순 정리한 뒤 항목을 모두 지운다.
        for (auto& entry : m_Rigs) ReleaseRig(entry.second);
        m_Rigs.clear();
    }

    // 등록 세대를 읽으며 미등록이면 0이다.
    uint64_t SkeletalPhysicsSystem::GetRigGeneration(ObjectId owner) const
    {
        // owner 항목을 찾아 세대를 반환한다.
        const auto found = m_Rigs.find(owner);
        return found == m_Rigs.end() ? 0 : found->second.Generation;
    }

    // 자산 body ID에 대응하는 runtime handle을 읽는다.
    MiniPhysics::BodyHandle SkeletalPhysicsSystem::GetBodyHandle(ObjectId owner, uint32_t bodyId) const
    {
        // owner와 body ID 범위를 확인한 뒤 handle을 반환한다.
        const auto found = m_Rigs.find(owner);
        if (found == m_Rigs.end() || bodyId >= found->second.Bodies.size()) return MiniPhysics::InvalidBody;
        return found->second.Bodies[bodyId];
    }

    // 자산 joint ID에 대응하는 runtime handle을 읽는다.
    MiniPhysics::ConstraintHandle SkeletalPhysicsSystem::GetConstraintHandle(ObjectId owner, uint32_t jointId) const
    {
        // owner와 joint ID 범위를 확인한 뒤 handle을 반환한다.
        const auto found = m_Rigs.find(owner);
        if (found == m_Rigs.end() || jointId >= found->second.Joints.size()) return MiniPhysics::InvalidConstraint;
        return found->second.Joints[jointId];
    }

    // 관절을 생성 역순으로 제거한 뒤 body를 역순으로 파괴한다.
    void SkeletalPhysicsSystem::ReleaseRig(Rig& rig)
    {
        // body보다 관절 참조를 먼저 world에서 제거한다.
        for (auto it = rig.Joints.rbegin(); it != rig.Joints.rend(); ++it) m_World.RemoveConstraint(*it);
        rig.Joints.clear();

        // body 파괴가 남은 연결 관절과 인접 pair 제외를 함께 정리한다.
        for (auto it = rig.Bodies.rbegin(); it != rig.Bodies.rend(); ++it) m_World.DestroyBody(*it);
        rig.Bodies.clear();
    }
}
