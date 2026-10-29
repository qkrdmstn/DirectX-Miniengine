#include "SkeletalPhysicsAsset.h"
#include <cmath>

namespace MiniPhysics
{
    // 강체 frame의 모든 성분과 단위 쿼터니언을 검사한다.
    static bool IsRigidFrame(const SkeletalRigidFrame& frame)
    {
        // 위치와 회전 성분의 비유한 값을 거절한다.
        for (double value : frame.Position)
        {
            if (!std::isfinite(value))
                return false;
        }
        double normSquared = 0;
        for (double value : frame.Rotation)
        {
            if (!std::isfinite(value))
                return false;
            normSquared += value * value;
        }

        // 입력을 자동 정규화하지 않고 제곱 길이 오차만 허용한다.
        return std::abs(normSquared - 1.0) <= 1.0e-4;
    }

    // 수치와 본 ancestry를 검증하여 연결된 단일 body tree만 허용한다.
    bool ValidateSkeletalPhysicsAsset(const SkeletalPhysicsAsset& asset,
        const std::vector<uint32_t>& nodeParents)
    {
        // 빈 자산과 ID로 표현할 수 없는 크기 및 잘못된 간선 수를 거절한다.
        if (asset.Bodies.empty() || asset.Bodies.size() >= InvalidSkeletalId ||
            nodeParents.empty() || nodeParents.size() >= InvalidSkeletalId ||
            asset.Joints.size() != asset.Bodies.size() - 1)
            return false;

        // 입력 부모 배열 전체의 범위와 순환을 먼저 확인한다.
        for (uint32_t node = 0; node < nodeParents.size(); ++node)
        {
            uint32_t ancestor = node;
            size_t depth = 0;
            while (ancestor != InvalidSkeletalId)
            {
                if (ancestor >= nodeParents.size() || ++depth > nodeParents.size())
                    return false;
                ancestor = nodeParents[ancestor];
            }
        }

        // 본마다 body 하나만 허용하고 형상과 질량 및 offset을 검사한다.
        std::vector<uint32_t> nodeBodies(nodeParents.size(), InvalidSkeletalId);
        for (uint32_t id = 0; id < asset.Bodies.size(); ++id)
        {
            const auto& body = asset.Bodies[id];
            if (body.Node >= nodeParents.size() || nodeBodies[body.Node] != InvalidSkeletalId ||
                !IsRigidFrame(body.BoneFromBody) || !std::isfinite(body.MassKg) || body.MassKg <= 0)
                return false;
            for (double extent : body.HalfExtentMeters)
            {
                if (!std::isfinite(extent) || extent <= 0)
                    return false;
            }
            nodeBodies[body.Node] = id;
        }

        // 관절의 endpoint와 각도 및 frame을 검사하고 중복 부모를 거절한다.
        constexpr double pi = 3.14159265358979323846;
        std::vector<uint32_t> bodyParents(asset.Bodies.size(), InvalidSkeletalId);
        for (const auto& joint : asset.Joints)
        {
            if (joint.ParentBody >= asset.Bodies.size() || joint.ChildBody >= asset.Bodies.size() ||
                joint.ParentBody == joint.ChildBody || bodyParents[joint.ChildBody] != InvalidSkeletalId ||
                !IsRigidFrame(joint.ParentBodyFromJoint) || !IsRigidFrame(joint.ChildBodyFromJoint) ||
                !std::isfinite(joint.TwistMin) || !std::isfinite(joint.TwistMax) ||
                joint.TwistMin < -pi || joint.TwistMax > pi || joint.TwistMin > joint.TwistMax)
                return false;
            for (double angle : joint.SwingHalfAngles)
            {
                if (!std::isfinite(angle) || angle < 0 || angle > pi)
                    return false;
            }

            // 모든 회전 자유도를 잠근 weld 설정은 지원하지 않는다.
            if (joint.SwingHalfAngles[0] == 0 && joint.SwingHalfAngles[1] == 0 &&
                joint.TwistMin == joint.TwistMax)
                return false;
            bodyParents[joint.ChildBody] = joint.ParentBody;
        }

        // 가장 가까운 body 조상과만 연결하며 root는 정확히 하나여야 한다.
        size_t roots = 0;
        for (uint32_t id = 0; id < asset.Bodies.size(); ++id)
        {
            uint32_t ancestor = nodeParents[asset.Bodies[id].Node];
            while (ancestor != InvalidSkeletalId && nodeBodies[ancestor] == InvalidSkeletalId)
                ancestor = nodeParents[ancestor];
            const uint32_t expected = ancestor == InvalidSkeletalId ? InvalidSkeletalId : nodeBodies[ancestor];
            if (bodyParents[id] != expected)
                return false;
            if (expected == InvalidSkeletalId)
                ++roots;
        }
        return roots == 1;
    }
}
