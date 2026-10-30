#include "PhysicsWorldInternal.h"
#include <cfloat>
#include <cmath>

namespace MiniPhysics
{
    namespace
    {
        struct ActivationSnapshot
        {
            BodyHandle Handle;
            JPH::BodyID Id;
            JPH::RVec3 Position;
            JPH::Quat Rotation;
            JPH::Vec3 Linear, Angular, ShapeCom;
            JPH::Mat44 LocalInverseInertia;
            float InverseMass;
            bool Transition;
        };

        // 배열 성분이 float 범위 안인지 검사하고 위치에는 별도 상한을 적용한다.
        template<size_t N>
        bool FiniteValues(const std::array<double, N>& values, double limit = FLT_MAX)
        {
            // 각 성분을 변환 전에 검사한다.
            for (double value : values)
                if (!std::isfinite(value) || std::abs(value) > limit) return false;
            return true;
        }

        // double 길이로 Jolt 속도 상한을 적용해 float 제곱의 overflow를 피한다.
        JPH::Vec3 ClampedVelocity(const std::array<double, 3>& values, float maximum)
        {
            // 기존 SetBodyVelocity와 같은 순서로 제한 비율을 계산한다.
            const double length = std::sqrt(values[0] * values[0] + values[1] * values[1] + values[2] * values[2]);
            const double factor = length > maximum ? maximum / length : 1.0;
            return JPH::Vec3(float(values[0] * factor), float(values[1] * factor), float(values[2] * factor));
        }

        // 현재 Dynamic 상태 또는 전환할 Kinematic 목표를 변경 없이 읽는다.
        bool ReadActivation(PhysicsWorldAccess::ImplType& impl, const BodyActivation& input, ActivationSnapshot& output)
        {
            // 살아 있는 비정적 body와 전환 가능한 저장 질량을 먼저 확인한다.
            const auto found = impl.SimBodies.find(input.Body);
            if (found == impl.SimBodies.end()) return false;
            const JPH::BodyLockRead lock(impl.World.GetBodyLockInterface(), found->second);
            if (!lock.Succeeded()) return false;
            const auto& body = lock.GetBody();
            if (!body.IsInBroadPhase() || body.IsStatic()) return false;
            const auto* motion = body.GetMotionProperties();
            output.Handle = input.Body;
            output.Id = found->second;
            output.Transition = body.IsKinematic();
            output.ShapeCom = body.GetShape()->GetCenterOfMass();
            output.InverseMass = motion->GetInverseMassUnchecked();
            output.LocalInverseInertia = motion->GetLocalSpaceInverseInertiaUnchecked();
            if (!std::isfinite(output.InverseMass) || output.InverseMass <= 0) return false;

            // 이미 Dynamic인 body는 입력 목표를 쓰지 않고 현재 pose와 속도를 보존한다.
            if (!output.Transition)
            {
                output.Position = body.GetPosition();
                output.Rotation = body.GetRotation();
                output.Linear = body.GetLinearVelocity();
                output.Angular = body.GetAngularVelocity();
                return true;
            }

            // Kinematic 목표만 검증하고 전환 직후 복구할 제한된 COM 속도를 준비한다.
            const auto& frame = input.Target;
            if (!FiniteValues(frame.Position, PhysicsWorld::MaxBodyPosition) || !FiniteValues(frame.Rotation) ||
                !FiniteValues(input.Linear) || !FiniteValues(input.Angular)) return false;
            double norm = 0;
            for (double value : frame.Rotation) norm += value * value;
            if (!std::isfinite(norm) || std::abs(norm - 1.0) > 1e-4) return false;
            output.Position = JPH::RVec3(float(frame.Position[0]), float(frame.Position[1]), float(frame.Position[2]));
            output.Rotation = JPH::Quat(float(frame.Rotation[0]), float(frame.Rotation[1]), float(frame.Rotation[2]), float(frame.Rotation[3])).Normalized();
            output.Linear = ClampedVelocity(input.Linear, motion->GetMaxLinearVelocity());
            output.Angular = ClampedVelocity(input.Angular, motion->GetMaxAngularVelocity());
            return true;
        }

        // 전환할 목표와 현재 Dynamic pose에서 내부 관절의 anchor와 실제 Jolt 제한을 검사한다.
        bool SupportsJointPose(const JPH::SwingTwistConstraint& joint, const ActivationSnapshot& parent, const ActivationSnapshot& child)
        {
            // Jolt가 저장한 COM anchor에 shape COM을 더해 body 원점 기준으로 비교한다.
            const auto a = parent.Position + parent.Rotation * (parent.ShapeCom + joint.GetLocalSpacePosition1());
            const auto b = child.Position + child.Rotation * (child.ShapeCom + joint.GetLocalSpacePosition2());
            if (JPH::Vec3(a - b).LengthSq() > 1e-8f) return false;

            // solver와 같은 cone 및 twist 제한으로 검사하되 pose를 보정하지 않는다.
            const auto relative = ((parent.Rotation * joint.GetConstraintToBody1()).Conjugated() *
                (child.Rotation * joint.GetConstraintToBody2())).Normalized();
            JPH::Quat swing, twist;
            relative.GetSwingTwist(swing, twist);
            JPH::SwingTwistConstraintPart limits;
            limits.SetLimits(joint.GetTwistMinAngle(), joint.GetTwistMaxAngle(),
                -joint.GetPlaneHalfConeAngle(), joint.GetPlaneHalfConeAngle(),
                -joint.GetNormalHalfConeAngle(), joint.GetNormalHalfConeAngle());
            auto clampedSwing = swing;
            auto clampedTwist = twist;
            JPH::uint flags = 0;
            limits.ClampSwingTwist(clampedSwing, clampedTwist, flags);
            return swing.IsClose(clampedSwing, 1e-12f) && twist.IsClose(clampedTwist, 1e-12f);
        }
    }

    // scene 스레드에서 전체 요청을 검증한 뒤 할당이나 실패 분기 없이 관절 분리를 게시한다.
    ConstraintBreakStatus PhysicsWorld::ApplyConstraintBreak(const ConstraintBreakDesc& desc)
    {
        // 기본 요청 값과 선택 관절의 수명을 검사한다.
        if (!m_Impl || !desc.Remove || desc.Bodies.empty()) return ConstraintBreakStatus::NotReady;
        if (!FiniteValues(desc.Impulse) || !FiniteValues(desc.Point, MaxBodyPosition)) return ConstraintBreakStatus::InvalidInput;
        const auto selected = m_Impl->Constraints.find(desc.Remove);
        if (selected == m_Impl->Constraints.end()) return ConstraintBreakStatus::NotReady;
        if (selected->second.Child != desc.ImpulseBody) return ConstraintBreakStatus::UnsupportedTopology;

        // 중복 없는 전체 body snapshot을 미리 확보하고 이 뒤에도 아직 world는 바꾸지 않는다.
        std::vector<ActivationSnapshot> snapshots(desc.Bodies.size());
        for (size_t i = 0; i < desc.Bodies.size(); ++i)
        {
            for (size_t j = 0; j < i; ++j)
                if (desc.Bodies[i].Body == desc.Bodies[j].Body) return ConstraintBreakStatus::UnsupportedTopology;
            if (!ReadActivation(*m_Impl, desc.Bodies[i], snapshots[i])) return ConstraintBreakStatus::InvalidInput;
        }
        const auto findBody = [&](BodyHandle handle) -> const ActivationSnapshot*
        {
            // 이미 확보한 snapshot만 검색하므로 commit 직전 추가 할당은 없다.
            for (const auto& snapshot : snapshots)
                if (snapshot.Handle == handle) return &snapshot;
            return nullptr;
        };
        const auto* impulseBody = findBody(desc.ImpulseBody);
        if (!impulseBody || findBody(selected->second.Parent)) return ConstraintBreakStatus::UnsupportedTopology;

        // 선택 관절의 반대 endpoint도 실제로 존재해야 분리와 wake가 안전하다.
        BodyActivation parentInput;
        parentInput.Body = selected->second.Parent;
        ActivationSnapshot parentSnapshot;
        if (!ReadActivation(*m_Impl, parentInput, parentSnapshot)) return ConstraintBreakStatus::NotReady;

        // enable 목록은 유효한 내부 관절만 한 번씩 포함하며 선택 관절을 다시 켤 수 없다.
        for (size_t i = 0; i < desc.Enable.size(); ++i)
        {
            if (desc.Enable[i] == desc.Remove ||
                std::find(desc.Enable.begin(), desc.Enable.begin() + i, desc.Enable[i]) != desc.Enable.begin() + i)
                return ConstraintBreakStatus::UnsupportedTopology;
            const auto found = m_Impl->Constraints.find(desc.Enable[i]);
            if (found == m_Impl->Constraints.end()) return ConstraintBreakStatus::NotReady;
            const auto* a = findBody(found->second.Parent);
            const auto* b = findBody(found->second.Child);
            if (!a || !b) return ConstraintBreakStatus::UnsupportedTopology;
            if (!SupportsJointPose(*found->second.Joint, *a, *b)) return ConstraintBreakStatus::UnsupportedPose;
        }

        // 다른 world 관절로 고정된 외부 pin과 빠뜨린 내부 관절을 거절한다.
        for (const auto& entry : m_Impl->Constraints)
        {
            if (entry.first == desc.Remove) continue;
            const bool a = findBody(entry.second.Parent) != nullptr;
            const bool b = findBody(entry.second.Child) != nullptr;
            if (a != b || (a && std::find(desc.Enable.begin(), desc.Enable.end(), entry.first) == desc.Enable.end()))
                return ConstraintBreakStatus::UnsupportedTopology;
        }

        // prospective COM·회전·속도와 저장 질량으로 impulse 결과의 float 범위를 먼저 검사한다.
        const JPH::Vec3 impulse(float(desc.Impulse[0]), float(desc.Impulse[1]), float(desc.Impulse[2]));
        const JPH::RVec3 point(float(desc.Point[0]), float(desc.Point[1]), float(desc.Point[2]));
        const auto center = impulseBody->Position + impulseBody->Rotation * impulseBody->ShapeCom;
        const auto rotation = JPH::Mat44::sRotation(impulseBody->Rotation);
        const auto inverseInertia = rotation.Multiply3x3(impulseBody->LocalInverseInertia).Multiply3x3RightTransposed(rotation);
        const auto linear = impulseBody->Linear + impulse * impulseBody->InverseMass;
        const auto angular = impulseBody->Angular + inverseInertia.Multiply3x3(JPH::Vec3(point - center).Cross(impulse));
        if (!std::isfinite(linear.LengthSq()) || !std::isfinite(angular.LengthSq())) return ConstraintBreakStatus::InvalidInput;

        // 선검증 이후에는 같은 scene 스레드의 저장 ID와 void Jolt 호출만 사용한다.
        auto& bodies = m_Impl->World.GetBodyInterface();
        m_Impl->World.RemoveConstraint(selected->second.Joint);
        m_Impl->Constraints.erase(selected);
        bodies.ActivateBody(parentSnapshot.Id);
        for (const auto& snapshot : snapshots)
        {
            if (snapshot.Transition)
            {
                bodies.SetPositionAndRotation(snapshot.Id, snapshot.Position, snapshot.Rotation, JPH::EActivation::DontActivate);
                bodies.SetMotionType(snapshot.Id, JPH::EMotionType::Dynamic, JPH::EActivation::Activate);
                bodies.SetLinearAndAngularVelocity(snapshot.Id, snapshot.Linear, snapshot.Angular);
            }
            bodies.ActivateBody(snapshot.Id);
        }
        for (const auto handle : desc.Enable)
        {
            auto& joint = m_Impl->Constraints.at(handle).Joint;
            joint->SetSwingMotorState(JPH::EMotorState::Off);
            joint->SetTwistMotorState(JPH::EMotorState::Off);
            joint->SetEnabled(true);
        }
        bodies.AddImpulse(impulseBody->Id, impulse, point);
        return ConstraintBreakStatus::Applied;
    }
}
