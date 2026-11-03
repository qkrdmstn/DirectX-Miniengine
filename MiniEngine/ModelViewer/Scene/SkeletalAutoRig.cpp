#include "SkeletalAutoRig.h"
#include "Scene.h"
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include "../../Model/json.hpp"
#include <functional>
#include <iomanip>

namespace Viewer
{
    // 원본 GLB의 이름을 ModelConvert::WalkGraph와 같은 깊이 우선 NodeId 순서로 읽는다.
    static std::vector<std::string> ReadDiagnosticNodeNames(const std::string& modelName)
    {
        // GLB JSON 청크를 읽어 기본 scene의 노드 계층을 확인한다.
        std::ifstream source("Models/" + modelName + ".glb", std::ios::binary);
        uint32_t header[5]{};
        source.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!source || header[0] != 0x46546C67 || header[4] != 0x4E4F534A)
            throw std::runtime_error("auto rig GLB names unavailable");
        std::string chunk(header[3], '\0');
        source.read(chunk.data(), chunk.size());
        const auto document = nlohmann::json::parse(chunk);

        // loader와 같은 자식 및 형제 순서로 이름을 평탄화한다.
        std::vector<std::string> names;
        // 노드를 먼저 기록하고 자식 subtree를 순서대로 방문한다.
        const std::function<void(unsigned)> visit = [&](unsigned index)
        {
            // 이름 없는 노드도 NodeId 순서를 보존한다.
            const auto& node = document.at("nodes").at(index);
            names.push_back(node.value("name", std::string("unnamed")));
            if (node.count("children"))
                for (const auto& child : node.at("children")) visit(child.get<unsigned>());
        };
        for (const auto& root : document.at("scenes").at(document.value("scene", 0)).at("nodes"))
            visit(root.get<unsigned>());
        return names;
    }

    // section E와 모델의 CPU 정점·inverse bind·팔레트·영향 플래그를 결합한다.
    bool BuildSkeletalPhysicsAsset(const Model& model, const SkeletalPoseSnapshot& pose,
        MiniPhysics::SkeletalPhysicsAsset& outAsset, unsigned* fallbackCount)
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
                if (section >= input.Sections.size() || section >= model.m_SkinCpuSections.size() ||
                    uint32_t(mesh->startJoint) + mesh->numJoints > model.m_NumJoints)
                    return RejectAutoRig("model section/palette count");
                auto& skin = input.Sections[section];
                skin.Vertices = &model.m_SkinCpuSections[section++];
                skin.InverseBind.assign(model.m_JointIBMs.get() + mesh->startJoint,
                    model.m_JointIBMs.get() + mesh->startJoint + mesh->numJoints);
                skin.Palette.assign(model.m_JointIndices.get() + mesh->startJoint,
                    model.m_JointIndices.get() + mesh->startJoint + mesh->numJoints);
            }
            mesh = reinterpret_cast<const Mesh*>(&mesh->draw[mesh->numDraws]);
        }
        if (section != input.Sections.size()) return RejectAutoRig("extra sections");
        return BuildSkeletalPhysicsAsset(input, outAsset, fallbackCount);
    }

    // 독립 pose의 자동 rig를 만들고 anchor·이름별 크기 CSV·CPU 메모리·fallback을 기록한다.
    void RunSkeletalAutoRigChecks(const Scene& scene)
    {
        // 로그는 TEMP에만 기록하고 장면의 pose와 물리 등록 상태는 건드리지 않는다.
        using namespace Math;
        std::ofstream log(std::filesystem::temp_directory_path() / "SkeletalAutoRigChecks.log");
        if (!log) throw std::runtime_error("auto rig log unavailable");
        log.precision(10);
        const auto directory = std::filesystem::temp_directory_path() / "SP7a3";
        std::filesystem::create_directories(directory);
        std::ofstream csv(directory / "BodyExtents.csv");
        if (!csv) throw std::runtime_error("auto rig CSV unavailable");
        csv.precision(10);
        csv << "Model,NodeId,JointName,HalfExtentX,HalfExtentY,HalfExtentZ\n";
        unsigned checked = 0;
        for (const auto& object : scene.GetObjects())
        {
            if (object->GetName() != "Xbot" && object->GetName() != "CesiumMan") continue;
            ++checked;
            const auto model = object->GetModelInstance()->GetModel();
            const auto names = ReadDiagnosticNodeNames(object->GetName());
            if (names.size() != model->m_NumNodes) throw std::runtime_error("auto rig name count mismatch");
            ModelInstance animation(model);
            animation.PlayAnimation(0, true);
            MiniPhysics::SkeletalPhysicsAsset asset;
            double maximum = 0;
            unsigned fallbacks = 0;
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
                if (!sample) passed = animation.HasValidPose() && BuildSkeletalPhysicsAsset(*model, pose, asset, &fallbacks);
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
            // 영구 CPU 복사본의 실제 capacity와 모든 축의 half extent 범위를 기록한다.
            size_t vertices = 0, bytes = model->m_SkinCpuSections.capacity() * sizeof(SkinCpuSection);
            for (const auto& section : model->m_SkinCpuSections)
            {
                vertices += section.size();
                bytes += section.capacity() * sizeof(SkinCpuVertex);
            }
            double minimumExtent = DBL_MAX, maximumExtent = 0;
            uint32_t maximumNode = 0;
            for (const auto& body : asset.Bodies)
            {
                csv << object->GetName() << ',' << body.Node << ',' << std::quoted(names.at(body.Node)) << ','
                    << body.HalfExtentMeters[0] << ',' << body.HalfExtentMeters[1] << ',' << body.HalfExtentMeters[2] << '\n';
                for (double extent : body.HalfExtentMeters)
                {
                    minimumExtent = (std::min)(minimumExtent, extent);
                    if (extent > maximumExtent)
                    {
                        maximumExtent = extent;
                        maximumNode = body.Node;
                    }
                }
            }
            log << object->GetName() << " cpuVertices=" << vertices << " cpuBytes=" << bytes
                << " minHalfExtentMeters=" << minimumExtent << " maxHalfExtentMeters=" << maximumExtent
                << " maxExtentJoint=" << names.at(maximumNode) << " maxExtentNode=" << maximumNode
                << " fallbackJoints=" << fallbacks << " bodies=" << asset.Bodies.size() << " joints=" << asset.Joints.size()
                << " Validate=" << (passed && MiniPhysics::ValidateSkeletalPhysicsAsset(asset, animation.GetPoseParents()) ? "PASS" : "FAIL")
                << " samples=5 maxAnchorErrorMeters=" << maximum << " registered=0\n";
            if (!passed) throw std::runtime_error("auto rig asset validation failed");
        }
        if (checked != 2) throw std::runtime_error("auto rig requires Xbot and CesiumMan");
    }
}
