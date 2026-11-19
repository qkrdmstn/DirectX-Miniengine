#include "SkeletalPhysicsSystem.h"
#include "../../Physics/Collision/AggregateGeom.h"
#include "SkeletalPhysicsFrames.h"
#include "../../Physics/Collision/BodyMass.h"
#include <algorithm>
#include <cmath>
#include <exception>

namespace Viewer
{
    // 선택 본에 body가 없으면 대체하지 않고 incoming 관절의 상태를 반환한다.
    IncomingJointResult SkeletalPhysicsSystem::FindIncomingJoint(ObjectId owner, uint32_t nodeId) const
    {
        // 미등록과 body 없는 본을 구분한다.
        const auto found = m_Rigs.find(owner);
        if (found == m_Rigs.end()) return {};
        const auto& rig = found->second;
        IncomingJointResult result;
        result.Status = IncomingJointStatus::NoBody;
        for (uint32_t b = 0; b < rig.Asset.Bodies.size(); ++b)
            if (rig.Asset.Bodies[b].Node == nodeId) result.ChildBody = b;
        if (result.ChildBody == UINT32_MAX) return result;

        // 자식 body가 일치하는 관절만 반환하며 root에는 incoming이 없다.
        result.Status = IncomingJointStatus::RigRoot;
        for (uint32_t j = 0; j < rig.Asset.Joints.size(); ++j)
            if (rig.Asset.Joints[j].ChildBody == result.ChildBody)
            {
                result.JointId = j;
                result.ParentBody = rig.Asset.Joints[j].ParentBody;
                result.ParentNode = rig.Asset.Bodies[result.ParentBody].Node;
                result.Status = rig.Joints[j] == MiniPhysics::InvalidConstraint
                    ? IncomingJointStatus::AlreadyBroken : IncomingJointStatus::Ok;
                break;
            }
        return result;
    }
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
            desc.Sensor = true;

            // 같은 owner에서 부착 해제된 조각과의 시작 겹침만 제외한다.
            desc.CollisionGroup = { owner, 0 };
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
            for (size_t i = 0; i < bodyWorldFrames.size(); ++i)
            {
                // 첫 갱신 전에도 물리 목표와 애니메이션 기준을 같은 초기 자세로 둔다.
                rig.Targets[i].Frame = rig.Targets[i].AnimationFrame = bodyWorldFrames[i];
            }
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
            targets[i].Frame = targets[i].AnimationFrame = frames[i];
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

            // Dynamic 물리 자세로 덮기 전에 선분 끝점에 쓸 애니메이션 자세를 보관한다.
            targets[i].AnimationFrame = targets[i].Frame;
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

    // 등록된 body handle의 소유자와 자산 번호를 찾고 실패하면 출력을 보존한다.
    bool SkeletalPhysicsSystem::FindBody(MiniPhysics::BodyHandle body, ObjectId& owner,
        uint32_t& bodyId, uint32_t& nodeId) const
    {
        // handle 번호 공간이 유일하므로 최초 일치 항목을 반환한다.
        if (body == MiniPhysics::InvalidBody) return false;
        for (const auto& entry : m_Rigs)
            for (uint32_t i = 0; i < entry.second.Bodies.size(); ++i)
                if (entry.second.Bodies[i] == body)
                {
                    owner = entry.first;
                    bodyId = i;
                    nodeId = entry.second.Asset.Bodies[i].Node;
                    return true;
                }
        return false;
    }

    // 선택 body와 incoming으로 후보를 판정하고 Kinematic 부모의 Dynamic 자식에만 애니메이션 끝점을 쓴다.
    void SkeletalPhysicsSystem::BuildKinematicSegments(ObjectId owner, std::vector<RigSegment>& out,
        std::vector<RigSegment>* unavailable) const
    {
        // 미등록 owner는 빈 목록을 반환한다.
        out.clear();
        if (unavailable) unavailable->clear();
        const auto found = m_Rigs.find(owner);
        if (found == m_Rigs.end()) return;
        const auto& rig = found->second;

        // 각 body 목표에서 월드 미터 단위 offset을 제거해 현재 본 원점을 복원한다.
        std::vector<Math::Vector3> origins(rig.Bodies.size(), Math::Vector3(Math::kZero));
        std::vector<Math::Vector3> endpoints = origins;
        std::vector<bool> kinematic(rig.Bodies.size(), false), valid(rig.Bodies.size(), false);
        std::vector<bool> endpointValid(rig.Bodies.size(), false);
        std::vector<bool> dynamic(rig.Bodies.size(), false);
        for (size_t i = 0; i < rig.Bodies.size(); ++i)
        {
            // 선분별 끝점 선택을 위해 body 상태와 애니메이션 원점을 보관한다.
            MiniPhysics::BodyMotion motion;
            Math::Matrix4 node;
            auto frame = rig.Targets[i].Frame;
            if (!m_World.GetBodyMotion(rig.Bodies[i], motion)) continue;
            kinematic[i] = motion == MiniPhysics::BodyMotion::Kinematic;
            dynamic[i] = motion == MiniPhysics::BodyMotion::Dynamic;
            const auto& endpoint = motion == MiniPhysics::BodyMotion::Dynamic ? rig.Targets[i].AnimationFrame : frame;
            if (SkeletalFrames::TryRestoreNodePose(rig.ModelToWorld, endpoint,
                rig.Asset.Bodies[i].BoneFromBody, rig.Targets[i].Scale, node))
            {
                endpoints[i] = Math::Vector3((rig.ModelToWorld * node).GetW());
                endpointValid[i] = true;
            }

            // 탈락 선분의 부모도 조회할 수 있도록 Dynamic 물리 원점을 따로 복원한다.
            if (motion != MiniPhysics::BodyMotion::Kinematic &&
                (!unavailable || !m_World.GetBodyPose(rig.Bodies[i], frame.Position, frame.Rotation))) continue;
            if (!SkeletalFrames::TryRestoreNodePose(rig.ModelToWorld, frame,
                    rig.Asset.Bodies[i].BoneFromBody, rig.Targets[i].Scale, node)) continue;
            origins[i] = Math::Vector3((rig.ModelToWorld * node).GetW());
            valid[i] = true;
        }

        // incoming이 있는 선택 본은 자신의 body와 incoming만 검사하고 root는 기존 조건을 쓴다.
        for (uint32_t i = 0; i < rig.Asset.Joints.size(); ++i)
        {
            const auto& joint = rig.Asset.Joints[i];
            const auto incoming = FindIncomingJoint(owner, rig.Asset.Bodies[joint.ParentBody].Node);
            const bool root = incoming.Status == IncomingJointStatus::RigRoot;
            const auto handle = root ? rig.Joints[i] : incoming.Status == IncomingJointStatus::Ok
                ? rig.Joints[incoming.JointId] : MiniPhysics::InvalidConstraint;
            MiniPhysics::ConstraintState state;
            const bool constraintFound = m_World.GetConstraintState(handle, state);

            // Kinematic 부모의 Dynamic 자식만 애니메이션 끝점을 쓰고 나머지는 물리 원점을 쓴다.
            const bool useAnimationEndpoint = kinematic[joint.ParentBody] && dynamic[joint.ChildBody];
            const auto& childOrigin = useAnimationEndpoint ? endpoints[joint.ChildBody] : origins[joint.ChildBody];
            const bool poseFound = valid[joint.ParentBody] &&
                (useAnimationEndpoint ? endpointValid[joint.ChildBody] : valid[joint.ChildBody]);
            const bool live = poseFound && constraintFound && kinematic[joint.ParentBody] &&
                (!root || kinematic[joint.ChildBody]);

            // 조회 실패도 탈락 목록에 보존하되 유효하지 않은 원점은 교차 판정에 쓰지 않는다.
            auto* destination = live ? &out : unavailable;
            if (destination) destination->push_back({ rig.Asset.Bodies[joint.ParentBody].Node, rig.Asset.Bodies[joint.ChildBody].Node,
                i, origins[joint.ParentBody], childOrigin, !poseFound,
                handle != MiniPhysics::InvalidConstraint && !constraintFound });
        }
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

    // scene 스레드의 step 바깥에서 실제 body와 관절 상태를 한 묶음으로 읽는다.
    bool SkeletalPhysicsSystem::ReadDiagnosticState(ObjectId owner, SkeletalRigState& output) const
    {
        // 미등록 owner와 오래된 handle은 호출자의 이전 결과를 보존한다.
        const auto found = m_Rigs.find(owner);
        if (found == m_Rigs.end()) return false;
        const auto& rig = found->second;
        SkeletalRigState result;
        result.Bodies.resize(rig.Bodies.size());
        result.Joints.resize(rig.Joints.size());
        std::vector<Math::Matrix4> matrices(rig.Bodies.size());

        // Kinematic 목표 대신 world의 실제 원점 pose와 COM 속도를 수집한다.
        for (size_t i = 0; i < rig.Bodies.size(); ++i)
        {
            auto& body = result.Bodies[i];
            body.Node = rig.Asset.Bodies[i].Node;
            const auto handle = rig.Bodies[i];
            if (!m_World.GetBodyPose(handle, body.Frame.Position, body.Frame.Rotation) ||
                !m_World.GetBodyCenterOfMass(handle, body.CenterOfMass) ||
                !m_World.GetBodyVelocity(handle, body.Linear, body.Angular) ||
                !m_World.GetBodyMotion(handle, body.Motion) ||
                !SkeletalFrames::TryRigidMatrix(body.Frame, matrices[i])) return false;
            for (size_t axis = 0; axis < 3; ++axis)
                if (!std::isfinite(body.CenterOfMass[axis]) || !std::isfinite(body.Linear[axis]) ||
                    !std::isfinite(body.Angular[axis])) return false;
        }

        // 끊긴 관절도 양쪽 anchor를 남겨 분리 전후 overlay에 사용할 수 있게 한다.
        for (size_t i = 0; i < rig.Joints.size(); ++i)
        {
            auto& state = result.Joints[i];
            const auto& joint = rig.Asset.Joints[i];
            state.Broken = rig.Joints[i] == MiniPhysics::InvalidConstraint;
            if (!state.Broken && !m_World.GetConstraintState(rig.Joints[i], state.Solver)) return false;
            Math::Matrix4 parent, child;
            if (!SkeletalFrames::TryRigidMatrix(joint.ParentBodyFromJoint, parent) ||
                !SkeletalFrames::TryRigidMatrix(joint.ChildBodyFromJoint, child)) return false;
            state.ParentAnchor = Math::Vector3((matrices[joint.ParentBody] * parent).GetW());
            state.ChildAnchor = Math::Vector3((matrices[joint.ChildBody] * child).GetW());
        }

        // 모든 조회가 성공한 경우에만 새 진단 상태를 게시한다.
        output = std::move(result);
        return true;
    }

    // 선택 관절 아래의 연결된 body를 검증하고 동기 분리를 게시한다.
    // 검증과 commit을 이어 부르는 얇은 진입점이다.
    BreakStatus SkeletalPhysicsSystem::BreakConstraint(const BreakRequest& request)
    {
        // 검증이 거절하면 그 사유를 그대로 돌려준다.
        PreparedBreak prepared;
        const auto status = PrepareBreak(request, prepared);
        if (status != BreakStatus::Prepared) return status;

        // 검증된 결과만 게시한다.
        return CommitBreak(std::move(prepared));
    }

    // 선택 관절 아래의 연결된 body를 rig와 world 변경 없이 검증한다.
    BreakStatus SkeletalPhysicsSystem::PrepareBreak(const BreakRequest& request, PreparedBreak& out) const
    {
        // 같은 관절 재요청은 오래된 세대나 잘못된 impulse보다 우선한다.
        const auto found = m_Rigs.find(request.Owner);
        if (found == m_Rigs.end()) return BreakStatus::NotFound;
        const auto& rig = found->second;
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

        // world의 전체 선검증만 실행하며 통과하면 commit 입력을 호출자에게 넘긴다.
        PreparedBreak prepared;
        const auto status = m_World.PrepareConstraintBreak(desc, prepared.World);
        switch (status)
        {
        case MiniPhysics::ConstraintBreakStatus::InvalidInput: return BreakStatus::InvalidInput;
        case MiniPhysics::ConstraintBreakStatus::NotReady: return BreakStatus::NotReady;
        case MiniPhysics::ConstraintBreakStatus::UnsupportedPose: return BreakStatus::UnsupportedPose;
        case MiniPhysics::ConstraintBreakStatus::UnsupportedTopology: return BreakStatus::UnsupportedTopology;
        case MiniPhysics::ConstraintBreakStatus::Applied: return BreakStatus::NotReady;
        case MiniPhysics::ConstraintBreakStatus::Prepared: break;
        }

        // 거절 경로가 모두 끝난 뒤에만 출력 객체를 채운다.
        prepared.Owner = request.Owner;
        prepared.JointId = request.JointId;
        prepared.RigGeneration = rig.Generation;
        prepared.TopologyGeneration = rig.TopologyGeneration;
        out = std::move(prepared);
        return BreakStatus::Prepared;
    }

    // 검증된 break를 world에 게시하고 논리 topology 세대를 올린다.
    // 준비 뒤 다른 break가 끼어 세대가 바뀌었으면 Stale로 거절한다.
    BreakStatus SkeletalPhysicsSystem::CommitBreak(PreparedBreak&& prepared)
    {
        // 단일 commit은 준비 시점의 topology 세대도 요구한다.
        return CommitPrepared(std::move(prepared), true);
    }

    // 게시 직전 조건을 world와 rig 변경 없이 확인한다.
    // 통과하면 Prepared를 돌려주며 batch 사전 점검과 commit이 함께 쓴다.
    BreakStatus SkeletalPhysicsSystem::CheckCommittable(const PreparedBreak& prepared, bool requireTopology) const
    {
        // rig가 같은 등록이고 관절이 아직 살아 있어야 한다.
        const auto found = m_Rigs.find(prepared.Owner);
        if (found == m_Rigs.end()) return BreakStatus::NotFound;
        const auto& rig = found->second;
        if (prepared.RigGeneration != rig.Generation) return BreakStatus::Stale;
        if (prepared.JointId >= rig.Joints.size()) return BreakStatus::NotFound;
        if (rig.Joints[prepared.JointId] == MiniPhysics::InvalidConstraint) return BreakStatus::AlreadyBroken;

        // 준비 뒤 다른 break가 끼었는지는 단일 commit에서만 확인한다.
        if (requireTopology && prepared.TopologyGeneration != rig.TopologyGeneration) return BreakStatus::Stale;

        // 준비한 관절과 world의 관절·body가 모두 남아 있어야 한다.
        if (prepared.World.GetRemovedConstraint() != rig.Joints[prepared.JointId]) return BreakStatus::NotReady;
        if (!m_World.CanCommitConstraintBreak(prepared.World)) return BreakStatus::NotReady;
        return BreakStatus::Prepared;
    }

    // 검사를 통과한 break만 world에 게시하고 성공마다 topology 세대를 1 올린다.
    BreakStatus SkeletalPhysicsSystem::CommitPrepared(PreparedBreak&& prepared, bool requireTopology)
    {
        // 거절 사유가 있으면 world와 rig를 바꾸지 않는다.
        const auto checked = CheckCommittable(prepared, requireTopology);
        if (checked != BreakStatus::Prepared) return checked;

        // world commit이 성공한 경우에만 논리 topology를 게시한다.
        auto& rig = m_Rigs.find(prepared.Owner)->second;
        if (m_World.CommitConstraintBreak(std::move(prepared.World)) != MiniPhysics::ConstraintBreakStatus::Applied)
            return BreakStatus::NotReady;
        rig.Joints[prepared.JointId] = MiniPhysics::InvalidConstraint;
        ++rig.TopologyGeneration;
        return BreakStatus::Applied;
    }

    // 두 검증된 break가 같은 관절이나 body를 건드리는지 본다.
    // 순서로 살리지 않고 겹치면 보수적으로 충돌로 본다.
    static bool BreaksConflict(const PreparedBreak& a, const PreparedBreak& b)
    {
        // 제거 관절끼리 같거나 한쪽 제거 관절이 다른 쪽의 Enable 목록에 있으면 충돌이다.
        const auto removeA = a.World.GetRemovedConstraint();
        const auto removeB = b.World.GetRemovedConstraint();
        const auto enableA = a.World.GetEnabledConstraints();
        const auto enableB = b.World.GetEnabledConstraints();
        if (removeA == removeB) return true;
        if (std::find(enableB.begin(), enableB.end(), removeA) != enableB.end()) return true;
        if (std::find(enableA.begin(), enableA.end(), removeB) != enableA.end()) return true;

        // 다시 켤 관절이 겹쳐도 충돌이다.
        for (const auto handle : enableA)
            if (std::find(enableB.begin(), enableB.end(), handle) != enableB.end()) return true;

        // 상태를 바꿀 body가 하나라도 겹치면 충돌이다.
        const auto bodiesA = a.World.GetBodies();
        const auto bodiesB = b.World.GetBodies();
        for (const auto body : bodiesA)
            if (std::find(bodiesB.begin(), bodiesB.end(), body) != bodiesB.end()) return true;
        return false;
    }

    // 요청 전체를 사전 검증하고 겹침이 없을 때만 순차 commit한다.
    // 하나라도 거절되면 아무것도 commit하지 않는다.
    bool SkeletalPhysicsSystem::BreakConstraints(const std::vector<BreakRequest>& requests, std::vector<BreakStatus>& statuses)
    {
        // 빈 batch는 할 일이 없으므로 거절한다.
        statuses.clear();
        if (requests.empty()) return false;

        // 모든 요청을 무변경으로 검증한다. 통과 요청은 임시로 Prepared를 기록한다.
        std::vector<PreparedBreak> prepared(requests.size());
        std::vector<BreakStatus> result(requests.size());
        bool rejected = false;
        for (size_t i = 0; i < requests.size(); ++i)
        {
            result[i] = PrepareBreak(requests[i], prepared[i]);
            if (result[i] != BreakStatus::Prepared) rejected = true;
        }

        // 검증된 요청 쌍의 겹침을 검사하고 겹친 양쪽을 InvalidInput으로 표시한다.
        for (size_t i = 0; i < requests.size(); ++i)
        {
            for (size_t j = i + 1; j < requests.size(); ++j)
            {
                if (!prepared[i].World.IsValid() || !prepared[j].World.IsValid() ||
                    !BreaksConflict(prepared[i], prepared[j])) continue;
                result[i] = BreakStatus::InvalidInput;
                result[j] = BreakStatus::InvalidInput;
                rejected = true;
            }
        }

        // 게시 직전 조건을 batch 전체에 대해 먼저 확인해 world가 반쯤 바뀌지 않게 한다.
        for (size_t i = 0; i < requests.size() && !rejected; ++i)
        {
            const auto checked = CheckCommittable(prepared[i], true);
            if (checked != BreakStatus::Prepared)
            {
                result[i] = checked;
                rejected = true;
            }
        }

        // 거절이 하나라도 있으면 통과한 요청에 batch 거절을 기록하고 아무것도 commit하지 않는다.
        if (rejected)
        {
            for (auto& status : result)
                if (status == BreakStatus::Prepared) status = BreakStatus::BatchRejected;
            statuses = std::move(result);
            return false;
        }

        // 겹침이 없으므로 세대 일치를 다시 요구하지 않고 순차 commit한다.
        // 사전 점검을 통과한 뒤의 실패는 불변식 위반이라 그 상태를 그대로 보고한다.
        // 첫 실패에서 멈추고 남은 요청은 NotReady로 표시한다.
        bool applied = true;
        for (size_t i = 0; i < requests.size(); ++i)
        {
            if (!applied)
            {
                result[i] = BreakStatus::NotReady;
                continue;
            }
            result[i] = CommitPrepared(std::move(prepared[i]), false);
            if (result[i] != BreakStatus::Applied) applied = false;
        }
        statuses = std::move(result);
        return applied;
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

    // 등록 직후 진단에서 실제 Jolt sensor body 수를 읽는다.
    size_t SkeletalPhysicsSystem::GetSensorCount(ObjectId owner) const
    {
        // 미등록 owner는 센서가 없다.
        const auto found = m_Rigs.find(owner);
        if (found == m_Rigs.end()) return 0;

        // 각 body의 읽기 잠금은 조회 함수 안에서 끝난다.
        size_t count = 0;
        for (auto body : found->second.Bodies)
            if (m_World.IsSensor(body)) ++count;
        return count;
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
