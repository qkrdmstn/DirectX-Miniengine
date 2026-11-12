#include "SkeletalAutoRig.h"
#include "Scene.h"
#include <fstream>
#include <filesystem>
#include <stdexcept>
#include "../../Model/json.hpp"
#include <functional>
#include <iomanip>
#include <sstream>
#include "Utility.h"
#include "Util/CommandLineArg.h"

namespace Viewer
{
    // 원본 GLB의 이름을 ModelConvert::WalkGraph와 같은 깊이 우선 NodeId 순서로 읽는다.
    static std::vector<std::string> ReadDiagnosticNodeNames(const std::filesystem::path& modelPath)
    {
        // GLB JSON 청크를 읽어 기본 scene의 노드 계층을 확인한다.
        std::ifstream source(modelPath, std::ios::binary);
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

    struct RigPreloadBox
    {
        Math::Vector3 Center{ Math::kZero };
        Math::Vector3 Axes[3];
        std::array<double, 3> Half;
    };

    // OBB의 면 법선 6개와 모서리 외적 9개로 SAT를 검사하고 최대 축 침투를 반환한다.
    static bool OverlapRigBoxes(const RigPreloadBox& a, const RigPreloadBox& b, double& maximumDepth)
    {
        // 거의 평행한 외적은 생략하고 나머지 후보 축은 단위 길이로 투영한다.
        using namespace Math;
        Vector3 axes[15] = { a.Axes[0], a.Axes[1], a.Axes[2], b.Axes[0], b.Axes[1], b.Axes[2] };
        for (unsigned i = 0; i < 3; ++i)
            for (unsigned j = 0; j < 3; ++j) axes[6 + i * 3 + j] = Cross(a.Axes[i], b.Axes[j]);
        maximumDepth = 0;
        for (const auto& candidate : axes)
        {
            const double length = float(Length(candidate));
            if (length < 1e-6) continue;
            const Vector3 axis = candidate / float(length);
            double radius = 0;
            for (unsigned i = 0; i < 3; ++i)
                radius += a.Half[i] * std::abs(float(Dot(a.Axes[i], axis)))
                    + b.Half[i] * std::abs(float(Dot(b.Axes[i], axis)));
            const double depth = radius - std::abs(float(Dot(b.Center - a.Center, axis)));
            if (depth <= 0) return false;
            maximumDepth = (std::max)(maximumDepth, depth);
        }

        // 최대 축 침투를 겹침 순위 측정값으로 사용한다.
        return true;
    }

    // 등록에 사용한 frame 변환과 미터 단위 반크기로 겹침 통계를 출력한다.
    void LogRigPreload(const std::string& ownerName, const SkeletalPoseSnapshot& pose,
        const MiniPhysics::SkeletalPhysicsAsset& asset, double milliseconds)
    {
        // RegisterFromPose와 같은 배율 분리 후 offset을 적용하며 반크기를 재확대하지 않는다.
        using namespace Math;
        std::vector<RigPreloadBox> boxes;
        for (const auto& body : asset.Bodies)
        {
            MiniPhysics::SkeletalRigidFrame frame;
            Matrix4 world;
            float scale = 0;
            if (!SkeletalFrames::TryBuildBodyFrame(pose.Sections.front().E, pose.Nodes[body.Node],
                body.BoneFromBody, frame, scale) || !SkeletalFrames::TryRigidMatrix(frame, world))
                throw std::runtime_error("RigPreload invalid registered frame");
            boxes.push_back({ Vector3(world.GetW()),
                { Vector3(world.GetX()), Vector3(world.GetY()), Vector3(world.GetZ()) }, body.HalfExtentMeters });
        }

        // constraint로 직접 연결된 쌍만 인접으로 분류하고 모든 body 쌍을 한 번씩 검사한다.
        struct Pair { size_t A, B; double Depth; };
        std::vector<Pair> overlaps;
        size_t totals[2]{}, hits[2]{};
        for (size_t a = 0; a < boxes.size(); ++a)
            for (size_t b = a + 1; b < boxes.size(); ++b)
            {
                bool adjacent = false;
                for (const auto& joint : asset.Joints)
                    adjacent |= (joint.ParentBody == a && joint.ChildBody == b)
                        || (joint.ParentBody == b && joint.ChildBody == a);
                ++totals[adjacent];
                double depth = 0;
                if (!OverlapRigBoxes(boxes[a], boxes[b], depth)) continue;
                ++hits[adjacent];
                if (!adjacent) overlaps.push_back({ a, b, depth });
            }

        // GLB 이름을 loader 순서로 읽고 원본이 없으면 NodeId로 식별한다.
        std::wstring customModel;
        const std::filesystem::path modelPath = CommandLineArgs::GetString(L"model", customModel)
            ? std::filesystem::path(customModel) : std::filesystem::path("Models/" + ownerName + ".glb");
        std::vector<std::string> names;
        try
        {
            names = ReadDiagnosticNodeNames(modelPath);
            if (names.size() != pose.Nodes.size()) names.clear();
        }
        catch (const std::exception&)
        {
            names.clear();
        }

        // 최대 축 침투가 큰 비인접 쌍 다섯 개를 재현 가능한 동률 순서로 고른다.
        std::stable_sort(overlaps.begin(), overlaps.end(), [](const Pair& a, const Pair& b)
        {
            // 동률이면 원래 body ID 순서를 유지한다.
            return a.Depth > b.Depth;
        });
        std::ostringstream log;
        log << std::fixed << std::setprecision(6);
        log << "RigPreload owner=" << ownerName << " bodies=" << asset.Bodies.size()
            << " joints=" << asset.Joints.size() << " create_register_ms=" << milliseconds << '\n';
        log << "RigPreload owner=" << ownerName << " adjacent_overlap=" << hits[1] << '/' << totals[1]
            << " nonadjacent_overlap=" << hits[0] << '/' << totals[0] << '\n';
        for (size_t i = 0; i < (std::min)(size_t(5), overlaps.size()); ++i)
        {
            const auto& pair = overlaps[i];
            const auto a = asset.Bodies[pair.A].Node, b = asset.Bodies[pair.B].Node;
            log << "RigPreload owner=" << ownerName << " rank=" << i + 1
                << " nodes=" << (names.empty() ? "Node" + std::to_string(a) : names[a]) << '[' << a << "] / "
                << (names.empty() ? "Node" + std::to_string(b) : names[b]) << '[' << b << ']'
                << " max_axis_penetration_m=" << pair.Depth << '\n';
        }

        // 기존 진단 관행대로 debugger와 TEMP 파일에 같은 내용을 남긴다.
        WriteRigPreloadLogSafely([&]
        {
            // 진단 문자열 할당도 예외 경계 안에서 수행한다.
            return log.str();
        });
    }

    // 모든 rig 진단을 기록하고 출력 실패의 예외는 내부에서 흡수한다.
    void WriteRigPreloadLog(const std::string& text) noexcept
    {
        // 경로 조회와 파일 생성까지 같은 예외 경계에서 처리한다.
        try
        {
            // 기존 출력 관행과 append 파일 경로를 공유한다.
            Utility::Print(text.c_str());
            std::ofstream file(std::filesystem::temp_directory_path() / "RigPreload.log", std::ios::app);
            file << text;
        }
        catch (...)
        {
            // 진단 출력 실패가 호출자의 작업 흐름을 바꾸지 않게 한다.
        }
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
            const auto names = ReadDiagnosticNodeNames("Models/" + object->GetName() + ".glb");
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
