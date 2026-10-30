#include "SkeletalPhysicsSystem.h"
#include "../../Physics/Collision/AggregateGeom.h"
#include "SkeletalPhysicsFrames.h"
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
            rig.Asset = asset;
            rig.NodeParents = nodeParents;
            rig.Targets.resize(asset.Bodies.size());
            for (size_t i = 0; i < bodyWorldFrames.size(); ++i) rig.Targets[i].Frame = bodyWorldFrames[i];
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

    // 본 월드 frame의 배율을 분리한 초기 pose로 rig를 등록한다.
    bool SkeletalPhysicsSystem::RegisterFromPose(ObjectId owner, const MiniPhysics::SkeletalPhysicsAsset& asset,
        const std::vector<uint32_t>& nodeParents, const Math::Matrix4& modelToWorld,
        const std::vector<Math::Matrix4>& nodePoses)
    {
        // 공통 frame과 본 배열을 검사한 뒤 모든 초기 목표를 임시 공간에 만든다.
        MiniPhysics::SkeletalRigidFrame frame;
        float frameScale = 0;
        if (nodePoses.size() != nodeParents.size() ||
            !SkeletalFrames::TrySplitUniformFrame(modelToWorld, frame, frameScale)) return false;
        std::vector<SkeletalBodyTarget> targets(asset.Bodies.size());
        std::vector<MiniPhysics::SkeletalRigidFrame> frames(asset.Bodies.size());
        for (size_t i = 0; i < asset.Bodies.size(); ++i)
        {
            const auto& body = asset.Bodies[i];
            if (body.Node >= nodePoses.size() || !SkeletalFrames::TryBuildBodyFrame(modelToWorld,
                nodePoses[body.Node], body.BoneFromBody, frames[i], targets[i].Scale)) return false;
            targets[i].Frame = frames[i];
        }

        // 성공한 등록에만 이미 확보한 목표와 배율을 게시한다.
        if (!Register(owner, asset, nodeParents, frames)) return false;
        auto& rig = m_Rigs.at(owner);
        rig.Targets.swap(targets);
        rig.ModelToWorld = modelToWorld;
        rig.FrameScale = frameScale;
        return true;
    }

    // 다음 step의 Kinematic 목표와 COM 속도를 갱신하고 Dynamic body는 보존한다.
    bool SkeletalPhysicsSystem::UpdateKinematicTargets(ObjectId owner, const Math::Matrix4& modelToWorld,
        const std::vector<Math::Matrix4>& nodePoses, float fixedDt)
    {
        // owner·고정 시간·공통 frame의 등록 배율을 먼저 검사한다.
        const auto found = m_Rigs.find(owner);
        if (found == m_Rigs.end() || !std::isfinite(fixedDt) || fixedDt < 4.0e-18f) return false;
        auto& rig = found->second;
        MiniPhysics::SkeletalRigidFrame frame;
        float frameScale = 0;
        if (nodePoses.size() != rig.NodeParents.size() ||
            !SkeletalFrames::TrySplitUniformFrame(modelToWorld, frame, frameScale) ||
            std::abs(frameScale - rig.FrameScale) > rig.FrameScale * 1e-4f) return false;

        // 한 목표라도 틀리면 앞 body의 속도도 바꾸지 않도록 전체를 선검증한다.
        auto targets = rig.Targets;
        std::vector<MiniPhysics::BodyMotion> motions(rig.Bodies.size());
        for (size_t i = 0; i < rig.Bodies.size(); ++i)
        {
            const auto& body = rig.Asset.Bodies[i];
            float scale = 0;
            if (!SkeletalFrames::TryBuildBodyFrame(modelToWorld, nodePoses[body.Node], body.BoneFromBody,
                targets[i].Frame, scale) ||
                std::abs(scale - rig.Targets[i].Scale) > rig.Targets[i].Scale * 1e-4f ||
                !m_World.GetBodyMotion(rig.Bodies[i], motions[i]) || motions[i] == MiniPhysics::BodyMotion::Static) return false;
            if (motions[i] == MiniPhysics::BodyMotion::Dynamic &&
                (!m_World.GetBodyPose(rig.Bodies[i], targets[i].Frame.Position, targets[i].Frame.Rotation) ||
                !m_World.GetBodyVelocity(rig.Bodies[i], targets[i].Linear, targets[i].Angular))) return false;
            if (motions[i] == MiniPhysics::BodyMotion::Kinematic &&
                !m_World.CanMoveKinematic(rig.Bodies[i], targets[i].Frame.Position, targets[i].Frame.Rotation, fixedDt)) return false;
        }

        // scene 스레드에서 선검증한 Kinematic만 적용하고 Jolt가 계산한 COM 속도를 저장한다.
        for (size_t i = 0; i < rig.Bodies.size(); ++i)
        {
            if (motions[i] == MiniPhysics::BodyMotion::Dynamic) continue;
            if (!m_World.MoveKinematic(rig.Bodies[i], targets[i].Frame.Position, targets[i].Frame.Rotation, fixedDt) ||
                !m_World.GetBodyVelocity(rig.Bodies[i], targets[i].Linear, targets[i].Angular)) return false;
        }
        rig.Targets.swap(targets);
        rig.ModelToWorld = modelToWorld;
        return true;
    }

    // 저장한 body 목표와 COM 속도를 읽는다.
    bool SkeletalPhysicsSystem::GetBodyTarget(ObjectId owner, uint32_t bodyId, SkeletalBodyTarget& target) const
    {
        // owner와 body ID가 유효할 때만 출력에 복사한다.
        const auto found = m_Rigs.find(owner);
        if (found == m_Rigs.end() || bodyId >= found->second.Targets.size()) return false;
        target = found->second.Targets[bodyId];
        return true;
    }

    // Dynamic body의 world pose에서 등록 배율과 body offset을 복원해 본 override를 만든다.
    bool SkeletalPhysicsSystem::CollectPoseOverrides(ObjectId owner, const Math::Matrix4& modelToWorld,
        std::vector<NodePhysicsPose>& overrides) const
    {
        // 등록된 owner와 변경되지 않은 E 배율을 먼저 확인한다.
        const auto found = m_Rigs.find(owner);
        if (found == m_Rigs.end()) return false;
        const auto& rig = found->second;
        MiniPhysics::SkeletalRigidFrame frame;
        float frameScale = 0;
        if (!SkeletalFrames::TrySplitUniformFrame(modelToWorld, frame, frameScale) ||
            std::abs(frameScale - rig.FrameScale) > rig.FrameScale * 1e-4f) return false;

        // 모든 body handle을 검사하고 물리가 제어하는 본만 임시 배열에 모은다.
        std::vector<NodePhysicsPose> result;
        result.reserve(rig.Bodies.size());
        for (size_t i = 0; i < rig.Bodies.size(); ++i)
        {
            MiniPhysics::BodyMotion motion;
            if (!m_World.GetBodyMotion(rig.Bodies[i], motion) || motion == MiniPhysics::BodyMotion::Static) return false;
            if (motion != MiniPhysics::BodyMotion::Dynamic) continue;
            MiniPhysics::SkeletalRigidFrame body;
            Math::Matrix4 pose;
            if (!m_World.GetBodyPose(rig.Bodies[i], body.Position, body.Rotation) ||
                !SkeletalFrames::TryRestoreNodePose(modelToWorld, body, rig.Asset.Bodies[i].BoneFromBody,
                    rig.Targets[i].Scale, pose)) return false;
            result.push_back({ rig.Asset.Bodies[i].Node, pose });
        }

        // 전체 수집이 성공해야 빈 결과를 포함해 호출자 배열을 교체한다.
        overrides.swap(result);
        return true;
    }

    // 선택 관절 아래의 연결된 body만 검증하고 동기 분리를 게시한다.
    BreakStatus SkeletalPhysicsSystem::BreakConstraint(const BreakRequest& request)
    {
        // 같은 관절 재요청은 오래된 세대나 잘못된 impulse보다 우선한다.
        const auto found = m_Rigs.find(request.Owner);
        if (found == m_Rigs.end()) return BreakStatus::NotFound;
        auto& rig = found->second;
        if (request.JointId >= rig.Joints.size()) return BreakStatus::NotFound;
        if (rig.Joints[request.JointId] == MiniPhysics::InvalidConstraint) return BreakStatus::AlreadyBroken;
        if (request.ExpectedRigGeneration != rig.Generation ||
            request.ExpectedTopologyGeneration != rig.TopologyGeneration) return BreakStatus::Stale;
        for (size_t i = 0; i < 3; ++i)
            if (!std::isfinite(request.WorldImpulse[i]) || !std::isfinite(request.WorldPoint[i]))
                return BreakStatus::InvalidInput;

        // 이미 끊긴 경계는 건너지 않으며 자산 인덱스 순서에 의존하지 않고 subtree를 모은다.
        MiniPhysics::ConstraintBreakDesc desc;
        desc.Remove = rig.Joints[request.JointId];
        const auto child = rig.Asset.Joints[request.JointId].ChildBody;
        desc.ImpulseBody = rig.Bodies[child];
        std::vector<bool> included(rig.Bodies.size(), false);
        std::vector<uint32_t> pending{ child };
        included[child] = true;
        for (size_t i = 0; i < pending.size(); ++i)
        {
            for (size_t j = 0; j < rig.Joints.size(); ++j)
            {
                const auto& joint = rig.Asset.Joints[j];
                if (rig.Joints[j] == MiniPhysics::InvalidConstraint || joint.ParentBody != pending[i]) continue;
                if (included[joint.ChildBody]) return BreakStatus::UnsupportedTopology;
                included[joint.ChildBody] = true;
                pending.push_back(joint.ChildBody);
                desc.Enable.push_back(rig.Joints[j]);
            }
        }

        // 전체 저장 공간과 목표를 준비하며 이 구간에서는 world나 세대를 변경하지 않는다.
        desc.Bodies.reserve(pending.size());
        for (uint32_t body : pending)
        {
            const auto& target = rig.Targets[body];
            desc.Bodies.push_back({ rig.Bodies[body], target.Frame, target.Linear, target.Angular });
        }
        for (size_t i = 0; i < 3; ++i)
        {
            desc.Impulse[i] = request.WorldImpulse[i];
            desc.Point[i] = request.WorldPoint[i];
        }

        // world의 전체 선검증과 무실패 commit이 성공한 경우에만 논리 topology를 게시한다.
        const auto status = m_World.ApplyConstraintBreak(desc);
        switch (status)
        {
        case MiniPhysics::ConstraintBreakStatus::InvalidInput: return BreakStatus::InvalidInput;
        case MiniPhysics::ConstraintBreakStatus::NotReady: return BreakStatus::NotReady;
        case MiniPhysics::ConstraintBreakStatus::UnsupportedPose: return BreakStatus::UnsupportedPose;
        case MiniPhysics::ConstraintBreakStatus::UnsupportedTopology: return BreakStatus::UnsupportedTopology;
        case MiniPhysics::ConstraintBreakStatus::Applied: break;
        }
        rig.Joints[request.JointId] = MiniPhysics::InvalidConstraint;
        ++rig.TopologyGeneration;
        return BreakStatus::Applied;
    }

    // owner의 현재 topology 번호를 읽는다.
    uint64_t SkeletalPhysicsSystem::GetTopologyGeneration(ObjectId owner) const
    {
        // 등록되지 않은 owner에는 유효한 세대가 없다.
        const auto found = m_Rigs.find(owner);
        return found == m_Rigs.end() ? 0 : found->second.TopologyGeneration;
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
