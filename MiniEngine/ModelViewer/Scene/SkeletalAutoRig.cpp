#include "SkeletalAutoRig.h"
#include "Scene.h"
#include <fstream>
#include <filesystem>
#include <stdexcept>

namespace Viewer
{
    // 호출자가 제공한 section E와 모델의 실제 팔레트·영향 플래그를 결합한다.
    bool BuildSkeletalPhysicsAsset(const Model& model, const SkeletalPoseSnapshot& pose,
        MiniPhysics::SkeletalPhysicsAsset& outAsset)
    {
        // 모델과 snapshot의 node 및 section 순서가 일치해야 한다.
        if (pose.Nodes.size() != model.m_NumNodes) return RejectAutoRig("model node count");
        SkeletalPoseSnapshot input = pose;
        input.Influencing = model.m_InfluencingJoints;
        size_t section = 0;
        const auto* mesh = reinterpret_cast<const Mesh*>(model.m_MeshData.get());
        for (uint32_t i = 0; i < model.m_NumMeshes; ++i)
        {
            if (mesh->numJoints)
            {
                if (section >= input.Sections.size() || uint32_t(mesh->startJoint) + mesh->numJoints > model.m_NumJoints)
                    return RejectAutoRig("model section/palette count");
                input.Sections[section++].Palette.assign(model.m_JointIndices.get() + mesh->startJoint,
                    model.m_JointIndices.get() + mesh->startJoint + mesh->numJoints);
            }
            mesh = reinterpret_cast<const Mesh*>(&mesh->draw[mesh->numDraws]);
        }
        if (section != input.Sections.size()) return RejectAutoRig("extra sections");
        return BuildSkeletalPhysicsAsset(input, outAsset);
    }

    // 독립 애니메이션 pose에서 자동 rig를 만들고 등록 없이 anchor 오차를 기록한다.
    void RunSkeletalAutoRigChecks(const Scene& scene)
    {
        // 로그는 TEMP에만 기록하고 장면의 pose와 물리 등록 상태는 건드리지 않는다.
        using namespace Math;
        std::ofstream log(std::filesystem::temp_directory_path() / "SkeletalAutoRigChecks.log");
        if (!log) throw std::runtime_error("auto rig log unavailable");
        log.precision(10);
        unsigned checked = 0;
        for (const auto& object : scene.GetObjects())
        {
            if (object->GetName() != "Xbot" && object->GetName() != "CesiumMan") continue;
            ++checked;
            const auto model = object->GetModelInstance()->GetModel();
            ModelInstance animation(model);
            animation.PlayAnimation(0, true);
            MiniPhysics::SkeletalPhysicsAsset asset;
            double maximum = 0;
            bool passed = true;
            for (unsigned sample = 0; sample < 5; ++sample)
            {
                animation.Update(sample ? 0.125f : 0.0f);
                if (!animation.HasValidPose()) throw std::runtime_error("auto rig invalid pose");
                SkeletalPoseSnapshot pose;
                pose.Parents = animation.GetPoseParents();
                pose.Nodes.assign(animation.GetNodeTransforms(), animation.GetNodeTransforms() + model->m_NumNodes);

                // 각 skin section의 렌더 E를 독립 애니메이션 pose에서 복원한다.
                for (const auto& component : object->GetMeshComponents())
                    for (size_t s = 0; s < component->GetGeometry()->GetSections().size(); ++s)
                    {
                        const auto& mesh = component->GetGeometry()->GetMesh(s);
                        if (mesh.numJoints) pose.Sections.push_back({ Matrix4(component->GetWorldTransform(object->GetTransform())) *
                            Matrix4(Invert(component->GetDeformBinding().SourceFrame.ToAffine())) * pose.Nodes[mesh.meshCBV], {} });
                    }
                if (!sample) passed = animation.HasValidPose() && BuildSkeletalPhysicsAsset(*model, pose, asset);
                if (!passed) break;

                // 최초 자산의 offset을 유지하고 표본 pose의 body frame만 다시 평가한다.
                std::vector<Matrix4> bodies;
                for (const auto& body : asset.Bodies)
                {
                    MiniPhysics::SkeletalRigidFrame frame;
                    float scale = 0;
                    Matrix4 matrix;
                    if (!SkeletalFrames::TryBuildBodyFrame(pose.Sections.front().E, pose.Nodes[body.Node], body.BoneFromBody, frame, scale) ||
                        !SkeletalFrames::TryRigidMatrix(frame, matrix)) throw std::runtime_error("auto rig invalid body frame");
                    bodies.push_back(matrix);
                }

                // 같은 joint의 양쪽 anchor 월드 위치 차이를 미터로 누적한다.
                for (const auto& joint : asset.Joints)
                {
                    Matrix4 a, b;
                    if (!SkeletalFrames::TryRigidMatrix(joint.ParentBodyFromJoint, a) || !SkeletalFrames::TryRigidMatrix(joint.ChildBodyFromJoint, b))
                        throw std::runtime_error("auto rig invalid joint frame");
                    maximum = (std::max)(maximum, double(Length(Vector3((bodies[joint.ParentBody] * a).GetW()) - Vector3((bodies[joint.ChildBody] * b).GetW()))));
                }
                log << object->GetName() << " sample=" << sample << " maxAnchorErrorMeters=" << maximum << '\n';
            }
            log << object->GetName() << " bodies=" << asset.Bodies.size() << " joints=" << asset.Joints.size()
                << " Validate=" << (passed && MiniPhysics::ValidateSkeletalPhysicsAsset(asset, animation.GetPoseParents()) ? "PASS" : "FAIL")
                << " samples=5 maxAnchorErrorMeters=" << maximum << " registered=0\n";
            if (!passed) throw std::runtime_error("auto rig asset validation failed");
        }
        if (checked != 2) throw std::runtime_error("auto rig requires Xbot and CesiumMan");
    }
}
