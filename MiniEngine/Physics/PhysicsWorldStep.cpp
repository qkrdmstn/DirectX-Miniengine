#include "PhysicsWorldInternal.h"
#include <cmath>

namespace MiniPhysics
{
    // 올라간 body의 운동 방식을 읽고 Jolt 열거형은 외부로 전달하지 않는다.
    bool PhysicsWorld::GetBodyMotion(BodyHandle handle, BodyMotion& motion) const
    {
        // 존재하며 broadphase에 올라간 body만 읽는다.
        if (!m_Impl) return false;
        const auto found = m_Impl->SimBodies.find(handle);
        if (found == m_Impl->SimBodies.end()) return false;
        const auto& bodies = m_Impl->World.GetBodyInterface();
        if (!bodies.IsAdded(found->second)) return false;

        // SDK 운동 방식을 중립 값으로 옮긴다.
        const auto type = bodies.GetMotionType(found->second);
        motion = type == JPH::EMotionType::Static ? BodyMotion::Static :
            type == JPH::EMotionType::Kinematic ? BodyMotion::Kinematic : BodyMotion::Dynamic;
        return true;
    }

    // 잠긴 동적 body의 역질량을 실제 질량으로 변환한다.
    bool PhysicsWorld::GetBodyMass(BodyHandle handle, double& mass) const
    {
        // handle을 찾고 읽기 잠금 아래에서 동적 body인지 확인한다.
        if (!m_Impl) return false;
        const auto entry = m_Impl->SimBodies.find(handle);
        if (entry == m_Impl->SimBodies.end()) return false;
        const JPH::BodyLockRead lock(m_Impl->World.GetBodyLockInterface(), entry->second);
        if (!lock.Succeeded() || !lock.GetBody().IsDynamic()) return false;

        // solver가 사용하는 역질량을 읽어 반환한다.
        mass = 1.0 / double(lock.GetBody().GetMotionProperties()->GetInverseMass());
        return true;
    }

    namespace
    {
        // handle에 대응하는 Jolt BodyID를 찾는다. 등록되지 않은 handle이면 false다.
        bool FindId(const PhysicsWorldAccess::ImplType* impl, BodyHandle handle, JPH::BodyID& id)
        {
            // 초기화되지 않은 world는 실패로 돌려준다.
            if (!impl)
                return false;

            // handle을 조회하고 없으면 실패로 돌려준다.
            const auto entry = impl->SimBodies.find(handle);
            if (entry == impl->SimBodies.end())
                return false;

            // 찾은 BodyID를 호출자에게 넘긴다.
            id = entry->second;
            return true;
        }
    }

    // 충돌 step 1회로 simulation을 진행한다. OptimizeBroadPhase는 부르지 않는다. body를 하나씩 추가하는 규모에서는 증분 갱신으로 충분하고 호출 비용이 크다.
    bool PhysicsWorld::Step(float deltaSeconds)
    {
        // 초기화 여부와 dt 유효성을 확인한다.
        if (!m_Impl || !std::isfinite(deltaSeconds) || !(deltaSeconds > 0.0f))
            return false;

        // 한 번 갱신하고 Jolt 오류는 false로 알리되 world는 계속 쓴다.
        const JPH::EPhysicsUpdateError error = m_Impl->World.Update(deltaSeconds, 1, m_Impl->TempAllocator.get(), m_Impl->JobSystem.get());
        return error == JPH::EPhysicsUpdateError::None;
    }

    // body 원점의 월드 위치와 회전(xyzw)을 읽는다.
    bool PhysicsWorld::GetBodyPose(BodyHandle handle, std::array<double, 3>& position, std::array<double, 4>& rotation) const
    {
        // handle을 찾아 body를 잠근다.
        JPH::BodyID id;
        if (!FindId(m_Impl.get(), handle, id))
            return false;

        const JPH::BodyLockRead lock(m_Impl->World.GetBodyLockInterface(), id);
        if (!lock.Succeeded())
            return false;

        // 원점(질량 중심 아님) pose를 double로 옮긴다.
        const JPH::RVec3 p = lock.GetBody().GetPosition();
        const JPH::Quat q = lock.GetBody().GetRotation();
        position = { double(p.GetX()), double(p.GetY()), double(p.GetZ()) };
        rotation = { double(q.GetX()), double(q.GetY()), double(q.GetZ()), double(q.GetW()) };
        return true;
    }

    // body 질량 중심의 월드 위치를 읽는다.
    bool PhysicsWorld::GetBodyCenterOfMass(BodyHandle handle, std::array<double, 3>& position) const
    {
        // handle을 찾아 body를 잠근다.
        JPH::BodyID id;
        if (!FindId(m_Impl.get(), handle, id))
            return false;

        const JPH::BodyLockRead lock(m_Impl->World.GetBodyLockInterface(), id);
        if (!lock.Succeeded())
            return false;

        // 질량 중심 위치를 double로 옮긴다.
        const JPH::RVec3 c = lock.GetBody().GetCenterOfMassPosition();
        position = { double(c.GetX()), double(c.GetY()), double(c.GetZ()) };
        return true;
    }

    // body가 깨어 있는지 읽는다.
    bool PhysicsWorld::IsBodyAwake(BodyHandle handle) const
    {
        // handle을 찾아 body를 잠근다.
        JPH::BodyID id;
        if (!FindId(m_Impl.get(), handle, id))
            return false;

        const JPH::BodyLockRead lock(m_Impl->World.GetBodyLockInterface(), id);

        // 잠금에 성공했고 body가 활성이면 깨어 있다.
        return lock.Succeeded() && lock.GetBody().IsActive();
    }

    // body의 선속도와 각속도를 읽는다. 정적 body는 Jolt가 0을 돌려준다.
    bool PhysicsWorld::GetBodyVelocity(BodyHandle handle, std::array<double, 3>& linear, std::array<double, 3>& angular) const
    {
        // handle을 찾아 body를 잠근다.
        JPH::BodyID id;
        if (!FindId(m_Impl.get(), handle, id))
            return false;

        const JPH::BodyLockRead lock(m_Impl->World.GetBodyLockInterface(), id);
        if (!lock.Succeeded())
            return false;

        // 속도를 double로 옮긴다.
        const JPH::Vec3 v = lock.GetBody().GetLinearVelocity();
        const JPH::Vec3 w = lock.GetBody().GetAngularVelocity();
        linear = { double(v.GetX()), double(v.GetY()), double(v.GetZ()) };
        angular = { double(w.GetX()), double(w.GetY()), double(w.GetZ()) };
        return true;
    }
}
