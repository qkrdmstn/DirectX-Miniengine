#pragma once

#include "../Scene.h"
#include "Util/CommandLineArg.h"
#include "Utility.h"
#include <algorithm>
#include <cfloat>
#include <cstdarg>
#include <fstream>
#include <cmath>
#include <stdexcept>

namespace Viewer
{
    // 디버거 출력과 작업 디렉터리의 진단 파일에 같은 로그를 남긴다.
    inline void LogXbotArm(const char* format, ...)
    {
        // 가변 인자를 안전한 버퍼에 포맷한다.
        char buffer[512];
        va_list args;
        va_start(args, format);
        vsprintf_s(buffer, sizeof(buffer), format, args);
        va_end(args);

        // 실행 로그는 옵션이 켜진 경우에만 파일에 덧붙인다.
        Utility::Print(buffer);
        std::ofstream log("XbotArmChecks.log", std::ios::app);
        log << buffer;
        if (!log)
            throw std::runtime_error("Cannot write XbotArmChecks.log");
    }

    // 진단 실패 이유를 Startup의 정상 종료 처리 경계로 전달한다.
    inline void CheckXbotArm(bool valid, const char* reason)
    {
        // 잘못된 자산이나 수치를 성공으로 처리하지 않는다.
        if (!valid)
            throw std::runtime_error(reason);
    }

    struct XbotRange
    {
        float Min = FLT_MAX, Max = -FLT_MAX;

        // 유한 표본의 최솟값과 최댓값을 누적한다.
        void Add(float value)
        {
            // NaN이나 무한대는 측정 실패로 보고한다.
            CheckXbotArm(std::isfinite(value), "non-finite sample");
            Min = (std::min)(Min, value);
            Max = (std::max)(Max, value);
        }
    };

    // 열 길이와 정규화 열 내적의 최댓값을 측정하고 강체 프레임을 반환한다.
    inline Math::Matrix4 XbotRigid(const Math::Matrix4& frame, float lengths[3], float& shear)
    {
        // 각 열의 길이와 방향을 분리한다.
        using namespace Math;
        Vector3 axes[3] = { Vector3(frame.GetX()), Vector3(frame.GetY()), Vector3(frame.GetZ()) };
        for (unsigned i = 0; i < 3; ++i)
        {
            lengths[i] = float(Length(axes[i]));
            CheckXbotArm(std::isfinite(lengths[i]) && lengths[i] > 0, "invalid scale");
            axes[i] = axes[i] / lengths[i];
        }

        // 전단과 비균일 배율을 검사한 뒤 회전과 위치만 남긴다.
        shear = (std::max)({ std::abs(float(Dot(axes[0], axes[1]))),
            std::abs(float(Dot(axes[0], axes[2]))), std::abs(float(Dot(axes[1], axes[2]))) });
        const float spread = (std::max)({ lengths[0], lengths[1], lengths[2] })
            - (std::min)({ lengths[0], lengths[1], lengths[2] });
        CheckXbotArm(spread <= lengths[0] * 1e-4f && shear <= 1e-4f, "unsupported scale/shear");
        return Matrix4(axes[0], axes[1], axes[2], Vector3(frame.GetW()));
    }

    // 실제 Xbot의 분리 평가와 기존 wrapper 및 전체 팔레트의 동등성을 검증한다.
    inline void CheckXbotPoseFinalization(const std::shared_ptr<const Model>& model)
    {
        // 모든 행렬 성분을 정확히 비교하여 연산 순서 차이도 검출한다.
        const auto checkMatrix = [](const Math::Matrix4& a, const Math::Matrix4& b)
        {
            // 저장된 성분은 패딩 없이 값으로 비교한다.
            DirectX::XMFLOAT4X4 left, right;
            DirectX::XMStoreFloat4x4(&left, a);
            DirectX::XMStoreFloat4x4(&right, b);
            for (unsigned row = 0; row < 4; ++row)
                for (unsigned col = 0; col < 4; ++col)
                    CheckXbotArm(left.m[row][col] == right.m[row][col], "BC-03a matrix mismatch");
        };

        // 같은 클립을 별도 인스턴스에서 여러 시간 간격으로 진행한다.
        ModelInstance wrapper(model), split(model);
        wrapper.PlayAnimation(0, true);
        split.PlayAnimation(0, true);
        const float steps[] = { 0.0f, 1.0f / 60.0f, 1.0f / 30.0f, 0.25f };
        for (float dt : steps)
        {
            // 애니메이션 입력은 읽을 수 있지만 최종 pose는 아직 무효여야 한다.
            wrapper.Update(dt);
            split.EvaluateAnimation(dt);
            CheckXbotArm(wrapper.HasValidPose() && !split.HasValidPose(), "BC-03a phase validity");
            for (uint32_t node = 0; node < model->m_NumNodes; ++node)
                checkMatrix(split.GetNodeTransforms()[node], wrapper.GetNodeTransforms()[node]);
            CheckXbotArm(split.FinalizePose(nullptr, 0) && split.HasValidPose(), "BC-03a finalize failed");

            // 모든 node와 각 IBM을 쓰는 position·normal 팔레트를 비교한다.
            for (uint32_t node = 0; node < model->m_NumNodes; ++node)
                checkMatrix(split.GetNodeTransforms()[node], wrapper.GetNodeTransforms()[node]);
            for (uint32_t joint = 0; joint < model->m_NumJoints; ++joint)
            {
                // normal 행렬은 4x4로 확장하여 유효한 세 축을 함께 검사한다.
                checkMatrix(split.GetJoints()[joint].posXform, wrapper.GetJoints()[joint].posXform);
                checkMatrix(Math::Matrix4(split.GetJoints()[joint].nrmXform),
                    Math::Matrix4(wrapper.GetJoints()[joint].nrmXform));
            }
        }

        // 중복 본이 있으면 해당 본을 override하여 모든 중복 entry 갱신을 확인한다.
        std::vector<uint32_t> entries(model->m_NumNodes, 0);
        uint32_t duplicates = 0, target = 13;
        for (uint32_t joint = 0; joint < model->m_NumJoints; ++joint)
            if (entries[model->m_JointIndices[joint]]++ != 0)
            {
                ++duplicates;
                target = model->m_JointIndices[joint];
            }
        const NodePhysicsPose overridePose = { target, Math::Matrix4(Math::Matrix3(Math::kIdentity),
            Math::Vector3(1.0f, 2.0f, 3.0f)) * split.GetNodeTransforms()[target] };
        CheckXbotArm(split.FinalizePose(&overridePose, 1), "BC-03a override failed");
        checkMatrix(split.GetNodeTransforms()[target], overridePose.ModelPose);
        for (uint32_t joint = 0; joint < model->m_NumJoints; ++joint)
        {
            // 중복 항목도 생략하지 않고 개별 IBM과 normal 변환의 기대값을 검사한다.
            const Math::Matrix4 expected = split.GetNodeTransforms()[model->m_JointIndices[joint]]
                * model->m_JointIBMs[joint];
            checkMatrix(split.GetJoints()[joint].posXform, expected);
            checkMatrix(Math::Matrix4(split.GetJoints()[joint].nrmXform),
                Math::Matrix4(Math::InverseTranspose(expected.Get3x3())));
        }

        // 잘못된 override는 출력 전체를 보존하고 최종 pose의 유효성만 해제해야 한다.
        const std::vector<Math::Matrix4> savedNodes(split.GetNodeTransforms(),
            split.GetNodeTransforms() + model->m_NumNodes);
        const std::vector<Joint> savedJoints(split.GetJoints(), split.GetJoints() + model->m_NumJoints);
        const NodePhysicsPose invalidPose = { UINT32_MAX, Math::Matrix4(Math::kIdentity) };
        CheckXbotArm(!split.FinalizePose(&invalidPose, 1) && !split.HasValidPose(), "BC-03a invalid override accepted");
        for (uint32_t node = 0; node < model->m_NumNodes; ++node)
            checkMatrix(split.GetNodeTransforms()[node], savedNodes[node]);
        for (uint32_t joint = 0; joint < model->m_NumJoints; ++joint)
        {
            // 실패 전 position과 normal 팔레트 모두 유지되는지 확인한다.
            checkMatrix(split.GetJoints()[joint].posXform, savedJoints[joint].posXform);
            checkMatrix(Math::Matrix4(split.GetJoints()[joint].nrmXform), Math::Matrix4(savedJoints[joint].nrmXform));
        }
        // 실제 자산의 skin 구성과 무관하게 동일 본을 참조하는 별도 CPU 모델을 만든다.
        auto fixture = std::make_shared<Model>();
        fixture->m_NumNodes = 3;
        fixture->m_NumMeshes = 0;
        fixture->m_NumAnimations = 0;
        fixture->m_NumJoints = 2;
        fixture->m_SceneGraph = std::make_unique<GraphNode[]>(3);
        fixture->m_JointIndices = std::make_unique<uint16_t[]>(2);
        fixture->m_JointIBMs = std::make_unique<Math::Matrix4[]>(2);
        for (uint32_t node = 0; node < 3; ++node)
        {
            // 세 본은 identity local을 가진 하나의 부모·자식 체인이다.
            GraphNode& graph = fixture->m_SceneGraph[node];
            graph.xform = Math::Matrix4(Math::kIdentity);
            graph.matrixIdx = node;
            graph.hasChildren = node < 2;
            graph.hasSibling = graph.skeletonRoot = graph.staleMatrix = 0;
        }
        fixture->m_JointIndices[0] = fixture->m_JointIndices[1] = 1;
        fixture->m_JointIBMs[0] = Math::Matrix4(Math::kIdentity);
        fixture->m_JointIBMs[1] = Math::Matrix4(Math::Matrix3(Math::kIdentity), Math::Vector3(3.0f, 0.0f, 0.0f));

        // 서로 다른 IBM을 가진 중복 팔레트가 모두 물리 pose를 반영해야 한다.
        ModelInstance duplicatePose(fixture);
        duplicatePose.EvaluateAnimation(0.0f);
        CheckXbotArm(!duplicatePose.HasValidPose(), "BC-03a synthetic phase validity");
        const NodePhysicsPose syntheticOverride = { 1, Math::Matrix4(Math::Matrix3::MakeScale(2.0f),
            Math::Vector3(1.0f, 2.0f, 3.0f)) };
        CheckXbotArm(duplicatePose.FinalizePose(&syntheticOverride, 1) && duplicatePose.HasValidPose(),
            "BC-03a synthetic override failed");
        for (uint32_t joint = 0; joint < 2; ++joint)
        {
            // 본 pose는 같아도 개별 IBM에 따른 position과 normal 기대값을 검사한다.
            const Math::Matrix4 expected = syntheticOverride.ModelPose * fixture->m_JointIBMs[joint];
            checkMatrix(duplicatePose.GetJoints()[joint].posXform, expected);
            checkMatrix(Math::Matrix4(duplicatePose.GetJoints()[joint].nrmXform),
                Math::Matrix4(Math::InverseTranspose(expected.Get3x3())));
        }
        LogXbotArm("BC-03a PASS: samples=4 nodes=%u palette=%u duplicateEntries=%u overrideNode=%u syntheticDuplicateEntries=2\n",
            model->m_NumNodes, model->m_NumJoints, duplicates, target);
    }

    // 옵션이 켜지면 별도 CPU 인스턴스로 Xbot 왼팔과 pose 확정을 검증하고 종료를 요청한다.
    inline bool RunXbotArmChecks(const Scene& scene)
    {
        // 기본 장면의 Xbot만 선택하여 이름 없는 런타임 자산의 구조를 확인한다.
        using namespace Math;
        uint32_t enabled = 0;
        if (!CommandLineArgs::GetInteger(L"xbot-arm-checks", enabled) || !enabled)
            return false;
        std::ofstream("XbotArmChecks.log", std::ios::trunc).close();
        const GameObject* xbot = nullptr;
        for (const auto& object : scene.GetObjects())
            if (object->GetName() == "Xbot")
                xbot = object.get();
        CheckXbotArm(xbot && xbot->GetModelInstance(), "default Xbot object required (omit -model)");
        const auto model = xbot->GetModelInstance()->GetModel();

        // DFS의 자식·형제 규칙으로 matrixIdx 기준 부모 맵을 복원한다.
        std::vector<uint32_t> parents(model->m_NumNodes, UINT32_MAX);
        uint32_t stack[32], depth = 0, parent = UINT32_MAX;
        for (uint32_t i = 0; i < model->m_NumNodes; ++i)
        {
            const GraphNode& node = model->m_SceneGraph[i];
            CheckXbotArm(node.matrixIdx < parents.size(), "invalid node index");
            parents[node.matrixIdx] = node.skeletonRoot ? UINT32_MAX : parent;
            if (node.hasChildren)
            {
                if (node.hasSibling)
                {
                    CheckXbotArm(depth < 32, "parent stack overflow");
                    stack[depth++] = parent;
                }
                parent = node.matrixIdx;
            }
            else if (!node.hasSibling)
                parent = depth ? stack[--depth] : UINT32_MAX;
        }
        LogXbotArm("BC-00 asset: nodes=%u expected=70; names unavailable; expected labels only: "
            "13 mixamorig:LeftArm -> 14 LeftForeArm -> 15 LeftHand\n", model->m_NumNodes);
        for (uint32_t id = 13; id <= 15 && id < parents.size(); ++id)
            LogXbotArm("BC-00 found: NodeId=%u parent=%u\n", id, parents[id]);
        CheckXbotArm(model->m_NumNodes == 70 && parents[13] == 12 && parents[14] == 13
            && parents[15] == 14 && model->m_NumAnimations > 0, "unexpected Xbot topology/animation");

        // 실제 자산으로 분리된 CPU pose 단계와 중복 팔레트 갱신을 회귀 검사한다.
        CheckXbotPoseFinalization(model);

        // 기본 장면과 같은 클립을 별도 인스턴스에서 고정 간격으로 재생한다.
        ModelInstance pose(model);
        pose.PlayAnimation(0, true);
        constexpr uint32_t ticks = 3600;
        constexpr float dt = 1.0f / 60.0f;
        CheckXbotArm(model->m_Animations[0].duration <= ticks * dt, "clip exceeds sampling window");
        LogXbotArm("BC-00 sampling: animation=0 duration=%.6f ticks=%u dt=%.9f; "
            "isolated CPU pose, scene unchanged; meters/radians\n", model->m_Animations[0].duration, ticks, dt);
        XbotRange columns[3][3], shears[3], bones[2], position[3], drift, swing, twist;
        Vector3 initialAnchor(kZero);
        uint32_t sectionCount = 0;
        for (uint32_t tick = 0; tick <= ticks; ++tick)
        {
            // 클립 시작 표본을 포함하고 매 tick 실제 렌더 section의 E를 다시 계산한다.
            pose.Update(tick ? dt : 0.0f);
            CheckXbotArm(pose.HasValidPose(), "invalid animation pose");
            const Matrix4* nodes = pose.GetNodeTransforms();
            Matrix4 commonE(kIdentity);
            bool found = false;
            uint32_t sections = 0;

            // 스킨 section을 순회하여 렌더러와 같은 월드 기준 E를 구한다.
            for (const auto& component : xbot->GetMeshComponents())
            {
                const auto& geometry = *component->GetGeometry();
                for (size_t section = 0; section < geometry.GetSections().size(); ++section)
                {
                    const Mesh& mesh = geometry.GetMesh(section);
                    if (!mesh.numJoints)
                        continue;
                    const Matrix4 e = Matrix4(component->GetWorldTransform(xbot->GetTransform()))
                        * Matrix4(Invert(component->GetDeformBinding().SourceFrame.ToAffine())) * nodes[mesh.meshCBV];
                    if (!tick)
                        LogXbotArm("BC-00 frame: section=%u Nmesh=NodeId %u Wc=component-world "
                            "SourceFrame=component-binding (exact renderer frame)\n", sections, mesh.meshCBV);

                    // 먼저 찾은 section의 E와 비교하여 공통 물리 frame인지 검증한다.
                    if (found)
                    {
                        const Matrix4 delta = Invert(commonE) * e;
                        DirectX::XMFLOAT4X4 values;
                        DirectX::XMStoreFloat4x4(&values, delta);
                        for (unsigned row = 0; row < 4; ++row)
                            for (unsigned col = 0; col < 4; ++col)
                                CheckXbotArm(std::abs(values.m[row][col] - (row == col ? 1.0f : 0.0f)) < 1e-4f,
                                    "skinned sections have different E");
                    }

                    // 다음 section 비교를 위해 공통 frame과 section 수를 갱신한다.
                    commonE = e;
                    found = true;
                    ++sections;
                }
            }

            // 유효한 스킨 section이 하나 이상 있는지 확인한다.
            CheckXbotArm(found, "no skinned section");
            sectionCount = sections;

            // 세 본의 월드 배율·전단과 강체 위치로 팔 길이를 측정한다.
            Matrix4 rigid[3];
            for (unsigned bone = 0; bone < 3; ++bone)
            {
                float lengths[3], shear;
                rigid[bone] = XbotRigid(commonE * nodes[13 + bone], lengths, shear);
                for (unsigned axis = 0; axis < 3; ++axis)
                    columns[bone][axis].Add(lengths[axis]);
                shears[bone].Add(shear);
                if (!tick)
                    LogXbotArm("BC-00 node=%u parent=%u columns=(%.9g %.9g %.9g) shear=%.9g\n",
                        13 + bone, parents[13 + bone], lengths[0], lengths[1], lengths[2], shear);
            }
            for (unsigned bone = 0; bone < 2; ++bone)
                bones[bone].Add(float(Length(Vector3(rigid[bone + 1].GetW()) - Vector3(rigid[bone].GetW()))));

            // 강체 전완 좌표의 손목 위치를 m로 기록하고 최초 anchor와의 거리를 누적한다.
            const Matrix4 relative = Invert(rigid[1]) * rigid[2];
            const Vector3 anchor(relative.GetW());
            if (!tick)
                initialAnchor = anchor;
            position[0].Add(float(anchor.GetX()));
            position[1].Add(float(anchor.GetY()));
            position[2].Add(float(anchor.GetZ()));
            drift.Add(float(Length(anchor - initialAnchor)));

            // 전완 로컬 +X를 twist 축으로 삼아 절대 상대 회전을 swing과 signed twist로 분해한다.
            DirectX::XMFLOAT4 q;
            DirectX::XMStoreFloat4(&q, DirectX::XMQuaternionNormalize(DirectX::XMQuaternionRotationMatrix(relative)));
            if (q.w < 0)
            {
                q.x = -q.x;
                q.y = -q.y;
                q.z = -q.z;
                q.w = -q.w;
            }
            const float twistNorm = std::sqrt(q.x * q.x + q.w * q.w);
            CheckXbotArm(twistNorm > 1e-6f, "singular swing/twist decomposition");
            twist.Add(2.0f * std::atan2(q.x, q.w));
            swing.Add(2.0f * std::acos((std::min)(1.0f, twistNorm)));
        }

        // 전체 재생 범위와 후속 rig 설정의 근거를 출력한다.
        for (unsigned bone = 0; bone < 3; ++bone)
            LogXbotArm("BC-00 range node=%u columns=[%.9g,%.9g] [%.9g,%.9g] [%.9g,%.9g] shear=[%.9g,%.9g]\n",
                13 + bone, columns[bone][0].Min, columns[bone][0].Max, columns[bone][1].Min,
                columns[bone][1].Max, columns[bone][2].Min, columns[bone][2].Max, shears[bone].Min, shears[bone].Max);
        LogXbotArm("BC-00 lengths m: shoulder-elbow=[%.9g,%.9g] elbow-wrist=[%.9g,%.9g]\n",
            bones[0].Min, bones[0].Max, bones[1].Min, bones[1].Max);
        LogXbotArm("BC-00 wrist forearm-local m: x=[%.9g,%.9g] y=[%.9g,%.9g] z=[%.9g,%.9g] drift=[%.9g,%.9g]\n",
            position[0].Min, position[0].Max, position[1].Min, position[1].Max,
            position[2].Min, position[2].Max, drift.Min, drift.Max);
        LogXbotArm("BC-00 rotation rad (+X twist, absolute local basis): swing=[%.9g,%.9g] twist=[%.9g,%.9g]\n",
            swing.Min, swing.Max, twist.Min, twist.Max);
        LogXbotArm("BC-00 PASS: sections=%u samples=%u; animation pose range (+0.05 rad margin): swing=[0,%.9g] "
            "twist=[%.9g,%.9g] anchorTolerance=%.9g m (drift proxy only)\n", sectionCount, ticks + 1,
            swing.Max + 0.05f, twist.Min - 0.05f, twist.Max + 0.05f, (std::max)(1e-4f, drift.Max * 2));
        return true;
    }
}
