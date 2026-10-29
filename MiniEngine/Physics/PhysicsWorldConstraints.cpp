#include "PhysicsWorldInternal.h"
#include <cmath>
#include <limits>
#include <Jolt/Physics/Body/BodyLockMulti.h>

namespace MiniPhysics
{
    // 유한한 body-local anchor와 단위 회전을 Jolt frame으로 변환한다.
    static bool ReadFrame(const SkeletalRigidFrame& frame, JPH::RVec3& position, JPH::Quat& rotation)
    {
        // float 기반 solver가 안전하게 처리하는 위치와 회전 범위를 검사한다.
        double length = 0;
        for (double value : frame.Position)
            if (!std::isfinite(value) || std::abs(value) > PhysicsWorld::MaxBodyPosition) return false;
        for (double value : frame.Rotation)
        {
            if (!std::isfinite(value)) return false;
            length += value * value;
        }
        if (!std::isfinite(length) || std::abs(length - 1.0) > 1e-4) return false;

        // 허용 오차 안의 회전을 정규화해 축을 직교 상태로 전달한다.
        position = JPH::RVec3(float(frame.Position[0]), float(frame.Position[1]), float(frame.Position[2]));
        rotation = JPH::Quat(float(frame.Rotation[0]), float(frame.Rotation[1]), float(frame.Rotation[2]), float(frame.Rotation[3])).Normalized();
        return true;
    }

    // 관절을 등록하기 전에 handle과 frame 및 각도 제한을 모두 검사한다.
    ConstraintHandle PhysicsWorld::CreateSwingTwist(const SwingTwistDesc& desc)
    {
        // 미등록·동일 endpoint와 handle 고갈은 아무 자원도 만들지 않는다.
        if (!m_Impl || desc.Parent == desc.Child || m_NextConstraintHandle == std::numeric_limits<ConstraintHandle>::max()) return InvalidConstraint;
        const auto parent = m_Impl->SimBodies.find(desc.Parent);
        const auto child = m_Impl->SimBodies.find(desc.Child);
        if (parent == m_Impl->SimBodies.end() || child == m_Impl->SimBodies.end()) return InvalidConstraint;
        auto& bodies = m_Impl->World.GetBodyInterface();
        if (!bodies.IsAdded(parent->second) || !bodies.IsAdded(child->second)) return InvalidConstraint;

        // 원점 기준 frame을 검증하고 각도 제한은 공통 자산 계약과 맞춘다.
        const auto& joint = desc.Joint;
        JPH::SwingTwistConstraintSettings settings;
        JPH::Quat q1, q2;
        if (!ReadFrame(joint.ParentBodyFromJoint, settings.mPosition1, q1) ||
            !ReadFrame(joint.ChildBodyFromJoint, settings.mPosition2, q2)) return InvalidConstraint;
        constexpr double Pi = 3.14159265358979323846;
        for (double angle : joint.SwingHalfAngles)
            if (!std::isfinite(angle) || angle < 0 || angle > Pi) return InvalidConstraint;
        if (!std::isfinite(joint.TwistMin) || !std::isfinite(joint.TwistMax) ||
            joint.TwistMin < -Pi || joint.TwistMax > Pi || joint.TwistMin > joint.TwistMax) return InvalidConstraint;
        if (joint.SwingHalfAngles[0] == 0 && joint.SwingHalfAngles[1] == 0 && joint.TwistMin == joint.TwistMax) return InvalidConstraint;

        // 두 body를 함께 잠그고 anchor를 body 원점에서 COM 기준으로 옮긴다.
        const JPH::BodyID ids[] = { parent->second, child->second };
        JPH::BodyLockMultiWrite locks(m_Impl->World.GetBodyLockInterface(), ids, 2);
        auto* a = locks.GetBody(0);
        auto* b = locks.GetBody(1);
        if (!a || !b || a->IsStatic() || b->IsStatic()) return InvalidConstraint;
        settings.mSpace = JPH::EConstraintSpace::LocalToBodyCOM;
        settings.mPosition1 -= JPH::RVec3(a->GetShape()->GetCenterOfMass());
        settings.mPosition2 -= JPH::RVec3(b->GetShape()->GetCenterOfMass());
        settings.mTwistAxis1 = q1 * JPH::Vec3::sAxisX();
        settings.mPlaneAxis1 = q1 * JPH::Vec3::sAxisY();
        settings.mTwistAxis2 = q2 * JPH::Vec3::sAxisX();
        settings.mPlaneAxis2 = q2 * JPH::Vec3::sAxisY();
        settings.mNormalHalfConeAngle = float(joint.SwingHalfAngles[0]);
        settings.mPlaneHalfConeAngle = float(joint.SwingHalfAngles[1]);
        settings.mTwistMinAngle = float(joint.TwistMin);
        settings.mTwistMaxAngle = float(joint.TwistMax);

        // map과 world가 각각 참조를 가지며 실패 시 map 참조를 되돌린다.
        JPH::Ref<JPH::SwingTwistConstraint> constraint = static_cast<JPH::SwingTwistConstraint*>(settings.Create(*a, *b));
        constraint->SetEnabled(a->IsDynamic() || b->IsDynamic());
        constraint->SetSwingMotorState(JPH::EMotorState::Off);
        constraint->SetTwistMotorState(JPH::EMotorState::Off);
        const bool enabled = constraint->GetEnabled();
        locks.ReleaseLocks();
        const auto handle = m_NextConstraintHandle++;
        m_Impl->Constraints.emplace(handle, ConstraintEntry{ desc.Parent, desc.Child, constraint });
        try
        {
            m_Impl->World.AddConstraint(constraint);
        }
        catch (...)
        {
            m_Impl->Constraints.erase(handle);
            throw;
        }
        SetConstraintEnabled(handle, enabled);
        return handle;
    }

    // Jolt world의 참조를 먼저 제거한 뒤 소유 map 참조를 해제한다.
    bool PhysicsWorld::RemoveConstraint(ConstraintHandle handle)
    {
        // stale handle은 재사용하지 않고 거절한다.
        if (!m_Impl) return false;
        const auto entry = m_Impl->Constraints.find(handle);
        if (entry == m_Impl->Constraints.end()) return false;
        m_Impl->World.RemoveConstraint(entry->second.Joint);

        // 잠든 Dynamic endpoint도 관절 해제 결과를 다음 step에 반영한다.
        auto& bodies = m_Impl->World.GetBodyInterface();
        bodies.ActivateBody(m_Impl->SimBodies.at(entry->second.Parent));
        bodies.ActivateBody(m_Impl->SimBodies.at(entry->second.Child));
        m_Impl->Constraints.erase(entry);
        return true;
    }

    // solver 참여를 바꾸고 비정적 endpoint를 깨우되 Dynamic endpoint 없는 관절은 켜지 않는다.
    bool PhysicsWorld::SetConstraintEnabled(ConstraintHandle handle, bool enabled)
    {
        // handle과 endpoint의 현재 운동 방식을 검사한다.
        if (!m_Impl) return false;
        const auto entry = m_Impl->Constraints.find(handle);
        if (entry == m_Impl->Constraints.end()) return false;
        auto& bodies = m_Impl->World.GetBodyInterface();
        const auto& value = entry->second;
        const auto a = m_Impl->SimBodies.at(value.Parent);
        const auto b = m_Impl->SimBodies.at(value.Child);
        if (enabled && (!bodies.IsAdded(a) || !bodies.IsAdded(b) ||
            (bodies.GetMotionType(a) != JPH::EMotionType::Dynamic && bodies.GetMotionType(b) != JPH::EMotionType::Dynamic))) return false;

        // enable과 disable 모두 비정적 endpoint를 활성화해 변경을 다음 step에 적용한다.
        value.Joint->SetEnabled(enabled);
        if (bodies.GetMotionType(a) != JPH::EMotionType::Static)
            bodies.ActivateBody(a);
        if (bodies.GetMotionType(b) != JPH::EMotionType::Static)
            bodies.ActivateBody(b);
        return true;
    }

    // 제한을 유지하면서 swing과 twist 모터만 끈다.
    bool PhysicsWorld::SetConstraintMotorsOff(ConstraintHandle handle)
    {
        // 유효 관절의 두 모터 상태를 함께 갱신한다.
        if (!m_Impl) return false;
        const auto entry = m_Impl->Constraints.find(handle);
        if (entry == m_Impl->Constraints.end()) return false;
        entry->second.Joint->SetSwingMotorState(JPH::EMotorState::Off);
        entry->second.Joint->SetTwistMotorState(JPH::EMotorState::Off);
        return true;
    }

    // Jolt 타입을 노출하지 않고 관절의 solver·motor 상태를 읽는다.
    bool PhysicsWorld::GetConstraintState(ConstraintHandle handle, ConstraintState& state) const
    {
        // 실패하면 호출자 출력은 그대로 둔다.
        if (!m_Impl) return false;
        const auto entry = m_Impl->Constraints.find(handle);
        if (entry == m_Impl->Constraints.end()) return false;
        const auto& joint = entry->second.Joint;
        state = { joint->GetEnabled(), joint->GetSwingMotorState() == JPH::EMotorState::Off && joint->GetTwistMotorState() == JPH::EMotorState::Off };
        return true;
    }

    // disabled를 포함한 살아 있는 관절 수를 반환한다.
    size_t PhysicsWorld::GetConstraintCount() const
    {
        // 초기화 전에는 자원이 없다.
        return m_Impl ? m_Impl->Constraints.size() : 0;
    }

    // rig 등록자가 지정한 직접 인접 pair만 body 수명 동안 충돌에서 제외한다.
    bool PhysicsWorld::ExcludeBodyPair(BodyHandle first, BodyHandle second)
    {
        // 등록된 서로 다른 비정적 body만 받는다.
        if (!m_Impl || first == second) return false;
        const auto a = m_Impl->SimBodies.find(first);
        const auto b = m_Impl->SimBodies.find(second);
        if (a == m_Impl->SimBodies.end() || b == m_Impl->SimBodies.end()) return false;
        auto& bodies = m_Impl->World.GetBodyInterface();
        if (!bodies.IsAdded(a->second) || !bodies.IsAdded(b->second) ||
            bodies.GetMotionType(a->second) == JPH::EMotionType::Static || bodies.GetMotionType(b->second) == JPH::EMotionType::Static) return false;

        // 할당 성공 후 필터를 게시하고 기존 접촉 캐시를 무효화한다.
        const auto x = a->second.GetIndexAndSequenceNumber();
        const auto y = b->second.GetIndexAndSequenceNumber();
        m_Impl->PairFilter->Pairs.insert(std::minmax(x, y));
        bodies.SetCollisionGroup(a->second, JPH::CollisionGroup(m_Impl->PairFilter, 0, x));
        bodies.SetCollisionGroup(b->second, JPH::CollisionGroup(m_Impl->PairFilter, 0, y));
        bodies.InvalidateContactCache(a->second);
        bodies.InvalidateContactCache(b->second);
        return true;
    }
}
