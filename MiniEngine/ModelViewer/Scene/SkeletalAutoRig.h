#pragma once
#include "SkeletalPhysicsFrames.h"
#include <cstdio>

class Model;
namespace Viewer
{
    class Scene;
    // Xbot과 CesiumMan의 자동 rig를 등록 없이 진단한다.
    void RunSkeletalAutoRigChecks(const Scene& scene);
    struct SkeletalSkinSection
    {
        Math::Matrix4 E{ Math::kIdentity };
        std::vector<uint32_t> Palette;
    };
    struct SkeletalPoseSnapshot
    {
        std::vector<uint32_t> Parents;
        std::vector<Math::Matrix4> Nodes;
        std::vector<SkeletalSkinSection> Sections;
        std::vector<uint8_t> Influencing;
    };

    // 실패 이유를 CPU 진단 스트림에 남긴다.
    inline bool RejectAutoRig(const char* reason)
    {
        // 실패한 입력은 출력 자산을 교체하지 않는다.
        std::fprintf(stderr, "SP-7a rejected: %s\n", reason);
        return false;
    }

    // CPU 정점이 없는 모델의 영향 본을 segment 기반 box와 SwingTwist 관절로 만든다.
    inline bool BuildSkeletalPhysicsAsset(const SkeletalPoseSnapshot& pose,
        MiniPhysics::SkeletalPhysicsAsset& outAsset)
    {
        // 크기와 전체 부모 그래프를 먼저 검증한다.
        using namespace Math;
        using namespace MiniPhysics;
        const size_t count = pose.Parents.size();
        if (!count || pose.Nodes.size() != count || pose.Influencing.size() != count || pose.Sections.empty())
            return RejectAutoRig("missing pose/skin");
        for (uint32_t node = 0; node < count; ++node)
        {
            size_t depth = 0;
            for (uint32_t p = node; p != UINT32_MAX; p = pose.Parents[p])
                if (p >= count || ++depth > count) return RejectAutoRig("invalid ancestry");
        }

        // 모든 section의 E 일치와 균일 배율을 확인하고 팔레트를 NodeId로 합친다.
        std::vector<uint32_t> ids(count, UINT32_MAX), ancestors(count, UINT32_MAX);
        std::vector<Matrix4> bones(count), bodies(count);
        const Matrix4 e = pose.Sections.front().E;
        SkeletalRigidFrame rigid;
        float scale = 0;
        for (const auto& section : pose.Sections)
        {
            if (!SkeletalFrames::TrySplitUniformFrame(section.E, rigid, scale))
                return RejectAutoRig("non-uniform section E");
            DirectX::XMFLOAT4X4 delta;
            DirectX::XMStoreFloat4x4(&delta, Invert(e) * section.E);
            for (unsigned r = 0; r < 4; ++r)
                for (unsigned c = 0; c < 4; ++c)
                    if (std::abs(delta.m[r][c] - (r == c ? 1.0f : 0.0f)) > 1e-4f)
                        return RejectAutoRig("section E mismatch");
            for (uint32_t node : section.Palette)
            {
                if (node >= count) return RejectAutoRig("palette range");
                if (pose.Influencing[node]) ids[node] = 0;
            }
        }

        // 가장 가까운 영향 조상을 연결하며 body root는 하나만 허용한다.
        SkeletalPhysicsAsset asset;
        unsigned roots = 0;
        for (uint32_t node = 0; node < count; ++node)
        {
            if (ids[node] == UINT32_MAX) continue;
            uint32_t parent = pose.Parents[node];
            while (parent != UINT32_MAX && ids[parent] == UINT32_MAX) parent = pose.Parents[parent];
            ancestors[node] = parent;
            roots += parent == UINT32_MAX;
            if (!SkeletalFrames::TrySplitUniformFrame(e * pose.Nodes[node], rigid, scale) ||
                !SkeletalFrames::TryRigidMatrix(rigid, bones[node])) return RejectAutoRig("non-uniform bone");
        }
        if (roots != 1) return RejectAutoRig("multiple/no influencing roots");

        // 가장 먼 영향 자식 방향을 X축으로 쓰고 말단은 들어오는 segment 길이를 사용한다.
        for (uint32_t node = 0; node < count; ++node)
        {
            if (ids[node] == UINT32_MAX) continue;
            const Vector3 origin(bones[node].GetW());
            Vector3 direction(bones[node].GetX());
            float length = 0;
            for (uint32_t child = 0; child < count; ++child)
                if (ids[child] != UINT32_MAX && ancestors[child] == node)
                {
                    const Vector3 segment = Vector3(bones[child].GetW()) - origin;
                    if (float(Length(segment)) > length)
                    {
                        length = Length(segment);
                        direction = segment;
                    }
                }
            const bool leaf = length <= 1e-6f;
            if (leaf && ancestors[node] != UINT32_MAX)
            {
                direction = origin - Vector3(bones[ancestors[node]].GetW());
                length = Length(direction);
            }
            if (length <= 1e-6f)
            {
                direction = Vector3(kXUnitVector);
                length = 0.05f;
            }
            const Vector3 x = Normalize(direction);
            const Vector3 reference = std::abs(float(x.GetY())) < 0.9f ? Vector3(kYUnitVector) : Vector3(kZUnitVector);
            const Vector3 z = Normalize(Cross(x, reference)), y = Cross(z, x);
            bodies[node] = Matrix4(Matrix3(x, y, z), leaf ? origin : origin + x * (length * 0.5f));
            BodyDefinition body;
            body.Node = node;
            if (!SkeletalFrames::TrySplitUniformFrame(Invert(bones[node]) * bodies[node], body.BoneFromBody, scale))
                return RejectAutoRig("invalid body offset");
            const double radius = (std::max)(0.005, double(length) * 0.15);
            body.HalfExtentMeters = { (std::max)(0.005, double(length) * 0.5), radius, radius };
            body.MassKg = 1000 * 8 * body.HalfExtentMeters[0] * radius * radius;
            ids[node] = uint32_t(asset.Bodies.size());
            asset.Bodies.push_back(body);
        }

        // 등록 pose의 자식 본 원점을 양쪽 body frame으로 옮겨 초기 anchor 오차를 없앤다.
        for (const auto& body : asset.Bodies)
        {
            const uint32_t parent = ancestors[body.Node];
            if (parent == UINT32_MAX) continue;
            JointDefinition joint;
            joint.ParentBody = ids[parent];
            joint.ChildBody = ids[body.Node];
            if (!SkeletalFrames::TrySplitUniformFrame(Invert(bodies[parent]) * bones[body.Node], joint.ParentBodyFromJoint, scale) ||
                !SkeletalFrames::TrySplitUniformFrame(Invert(bodies[body.Node]) * bones[body.Node], joint.ChildBodyFromJoint, scale))
                return RejectAutoRig("invalid anchor");
            joint.SwingHalfAngles = { 2.5, 2.5 };
            joint.TwistMin = -3.0;
            joint.TwistMax = 3.0;
            asset.Joints.push_back(joint);
        }
        if (!ValidateSkeletalPhysicsAsset(asset, pose.Parents)) return RejectAutoRig("asset validation");
        outAsset = std::move(asset);
        return true;
    }

    // 모델의 실제 skin 메타데이터와 등록 pose로 자동 물리 자산을 생성한다.
    bool BuildSkeletalPhysicsAsset(const Model& model, const SkeletalPoseSnapshot& pose,
        MiniPhysics::SkeletalPhysicsAsset& outAsset);
}
