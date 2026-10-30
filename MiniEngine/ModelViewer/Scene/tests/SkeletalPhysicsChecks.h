#pragma once

#include "../PhysicsSystem.h"
#include "../CollisionDebugMath.h"
#include "../GizmoDraw.h"
#include "../SkeletalPhysicsFrames.h"
#include "Camera.h"
#include "Utility.h"
#include <fstream>

namespace Viewer
{
    // fixture 또는 기존 Xbot의 세 상자 rig를 공통 프레임 경로로 검사한다.
    class SkeletalPhysicsChecks
    {
    public:
        bool Enabled = false;
        bool Done = false;
        bool Xbot = false;

        // 선택한 진단 모델에 rig를 등록하고 초기 실제 body 상태를 저장한다.
        void Start(Scene& scene, PhysicsSystem& physics)
        {
            // 진단 로그를 열고 기존 Xbot 모드이면 별도 등록 경로로 보낸다.
            if (!Enabled) return;
            m_Log.open("SkeletalPhysicsChecks.log", std::ios::trunc);
            if (Xbot)
            {
                StartXbot(scene, physics);
                return;
            }

            // fixture 모드는 원본 모델과 독립된 정지 local pose 네 개를 만든다.
            auto model = std::make_shared<Model>();
            model->m_NumNodes = 4;
            model->m_NumMeshes = model->m_NumJoints = model->m_NumAnimations = 0;
            model->m_SceneGraph = std::make_unique<GraphNode[]>(4);
            for (uint32_t i = 0; i < 4; ++i)
            {
                auto& node = model->m_SceneGraph[i];
                node.matrixIdx = i;
                node.hasChildren = i < 3;
                node.xform = Math::Matrix4(Math::OrthogonalTransform(Math::Quaternion(Math::kIdentity),
                    Math::Vector3(i == 0 ? -2.5f : 0, i == 0 ? 2.0f : -0.5f, 0)));
                node.rotation = Math::Quaternion(Math::kIdentity);
                node.scale = { 1, 1, 1 };
            }
            auto& object = scene.CreateObject("Skeletal break fixture", model);
            m_Owner = object.GetId();
            scene.InitializePoses();

            // 공통 world에 등록하고 실패는 진단 종료로 전달한다.
            auto fixture = MiniPhysics::CreateThreeBodyFixture();
            const auto* nodes = object.GetModelInstance()->GetNodeTransforms();
            auto& skeletal = physics.GetSkeletalPhysics();
            if (!physics.IsSimulationEnabled() || !skeletal.RegisterFromPose(m_Owner, fixture.Asset,
                fixture.NodeParents, Math::Matrix4(Math::kIdentity), std::vector<Math::Matrix4>(nodes, nodes + 4)) ||
                !skeletal.ReadDiagnosticState(m_Owner, m_State))
            {
                Finish(false);
                return;
            }
            m_Initial = m_State;
            m_Log << "BC-12a fixture bodies=3 joints=2 fixedDt=1/60 rootY=" << m_State.Bodies[0].CenterOfMass[1] << "\n";
        }

        // 60 tick 경계에서 요청을 넣어 다음 substep의 target 뒤에 분리한다.
        void BeforeStep(PhysicsSystem& physics)
        {
            // 진단 프레임은 fixed tick 하나만 소비하므로 경계를 건너뛰지 않는다.
            if (!Enabled || Done || m_Ticks != 60) return;
            auto& skeletal = physics.GetSkeletalPhysics();
            BreakRequest request{ m_Owner, 0, skeletal.GetRigGeneration(m_Owner),
                skeletal.GetTopologyGeneration(m_Owner) };
            if (Xbot)
            {
                request.WorldImpulse = { 0.05f, 0, 0 };
                for (unsigned i = 0; i < 3; ++i)
                    request.WorldPoint[i] = float(m_State.Bodies[1].CenterOfMass[i]);
            }
            physics.QueueBreak(request);
            m_BreakChildY = m_State.Bodies[1].CenterOfMass[1];
            m_Log << Label() << " break queued at tick=60 impulseX=" << request.WorldImpulse[0] << "\n";
        }

        // 실제 step 수와 pose를 읽고 분리 뒤 120 tick에서 결과를 기록한다.
        void AfterStep(const Scene& scene, PhysicsSystem& physics, unsigned steps)
        {
            // 모든 프레임에서 실제 body와 본 pose의 유한성을 누적 검사한다.
            if (!Enabled || Done) return;
            m_Ticks += steps;
            const auto* object = scene.FindObject(m_Owner);
            if (steps != 1 || !object || !physics.GetSkeletalPhysics().ReadDiagnosticState(m_Owner, m_State))
            {
                Finish(false);
                return;
            }
            const auto* pose = object->GetModelInstance();
            m_Finite &= pose->HasValidPose();
            for (uint32_t i = 0; i < pose->GetModel()->m_NumNodes; ++i)
            {
                const Math::Matrix4& matrix = pose->GetNodeTransforms()[i];
                const float* values = reinterpret_cast<const float*>(&matrix);
                for (unsigned j = 0; j < 16; ++j) m_Finite &= std::isfinite(values[j]);
            }

            // 별도 애니메이션 인스턴스의 model pose와 상완의 모든 성분을 비교한다.
            if (Xbot)
            {
                m_Animation->Update(1.0f / 60.0f);
                DirectX::XMFLOAT4X4 actual, expected;
                DirectX::XMStoreFloat4x4(&actual, pose->GetNodeTransforms()[13]);
                DirectX::XMStoreFloat4x4(&expected, m_Animation->GetNodeTransforms()[13]);
                for (unsigned row = 0; row < 4; ++row)
                    for (unsigned col = 0; col < 4; ++col)
                        m_AnimationError = std::max(m_AnimationError,
                            double(std::abs(actual.m[row][col] - expected.m[row][col])));
                m_Finite &= m_Animation->HasValidPose() && std::isfinite(m_AnimationError);
            }

            // 실제 강체 pose와 COM 및 두 속도의 유한성을 검사한다.
            for (const auto& body : m_State.Bodies)
            {
                for (double value : body.Frame.Position) m_Finite &= std::isfinite(value);
                for (double value : body.Frame.Rotation) m_Finite &= std::isfinite(value);
                for (double value : body.CenterOfMass) m_Finite &= std::isfinite(value);
                for (double value : body.Linear) m_Finite &= std::isfinite(value);
                for (double value : body.Angular) m_Finite &= std::isfinite(value);
            }

            // 바닥 근처에서 하위 상자의 수직 속도가 급증하면 최초 충돌 tick으로 고정한다.
            std::array<double, 3> floorCenter{}, floorHalf{};
            if (!physics.GetFloorBox(floorCenter, floorHalf))
            {
                Finish(false);
                return;
            }
            const double lowerY = m_State.Bodies[2].CenterOfMass[1];
            const double lowerVelocityY = m_State.Bodies[2].Linear[1];
            if (m_ContactTick == 0 && m_Ticks > 60 && lowerY <= floorCenter[1] + floorHalf[1] + 0.30 &&
                lowerVelocityY - m_PreviousLowerVelocityY > 1.0)
                m_ContactTick = m_Ticks;
            m_PreviousLowerVelocityY = lowerVelocityY;

            // 자유낙하와 최초 접촉 이후의 손목 오차를 따로 누적한다.
            const double error = double(Math::Length(m_State.Joints[1].ParentAnchor - m_State.Joints[1].ChildAnchor));
            if (error > m_MaxAnchorError)
            {
                m_MaxAnchorError = error;
                m_MaxErrorTick = m_Ticks;
            }
            m_Log.precision(10);
            m_Log << Label() << " sample tick=" << m_Ticks << " error=" << error
                << " childComY=" << m_State.Bodies[1].CenterOfMass[1]
                << " lowerComY=" << m_State.Bodies[2].CenterOfMass[1]
                << " lowerVelocityY=" << lowerVelocityY << " contactTick=" << m_ContactTick << "\n";
            m_Finite &= std::isfinite(error);
            if (m_ContactTick == 0) m_FreeFallMaxError = std::max(m_FreeFallMaxError, error);
            else m_ContactMaxError = std::max(m_ContactMaxError, error);

            // 마지막 tick에서 root 유지·자손 낙하와 구간별 손목 오차를 판정한다.
            if (m_Ticks != 180) return;
            const bool renderPose = !Xbot || CheckRenderPose(*object);
            const double rootDelta = m_State.Bodies[0].CenterOfMass[1] - m_Initial.Bodies[0].CenterOfMass[1];
            const double childDelta = m_State.Bodies[1].CenterOfMass[1] -
                (Xbot ? m_BreakChildY : m_Initial.Bodies[1].CenterOfMass[1]);
            const bool topology = m_State.Joints[0].Broken && !m_State.Joints[1].Broken &&
                m_State.Joints[1].Solver.Enabled && m_State.Joints[1].Solver.MotorsOff &&
                !m_Initial.Joints[0].Broken && !m_Initial.Joints[1].Broken &&
                !m_Initial.Joints[0].Solver.Enabled && !m_Initial.Joints[1].Solver.Enabled &&
                m_State.Bodies[0].Motion == MiniPhysics::BodyMotion::Kinematic &&
                m_State.Bodies[1].Motion == MiniPhysics::BodyMotion::Dynamic &&
                m_State.Bodies[2].Motion == MiniPhysics::BodyMotion::Dynamic;
            m_Log << Label() << " ticks=180 rootDeltaY=" << rootDelta << " childDeltaY=" << childDelta
                << " wristMaxError=" << m_MaxAnchorError << " maxErrorTick=" << m_MaxErrorTick
                << " contactTick=" << m_ContactTick << " freeFallMaxError=" << m_FreeFallMaxError
                << " contactMaxError=" << m_ContactMaxError << " finalError=" << error
                << " animationMaxError=" << m_AnimationError
                << " unbroken=2->" << (!m_State.Joints[0].Broken + !m_State.Joints[1].Broken)
                << " enabled=0->" << m_State.Joints[1].Solver.Enabled
                << " finite=" << m_Finite << " topology=" << topology << "\n";
            if (Xbot && m_FreeFallMaxError > 0.01) m_Log << "BC-12b BLOCKED pre-contact wrist error\n";
            Finish((Xbot ? m_AnimationError <= 1e-5 : std::abs(rootDelta) <= 1e-5) &&
                childDelta < -0.01 && (Xbot || m_ContactTick > 60) &&
                m_FreeFallMaxError <= 0.01 && m_ContactMaxError <= 0.02 && error <= 0.01 && m_Finite && topology && renderPose);
        }

        // 실제 COM·월드 본·양쪽 anchor를 카메라 근평면으로 잘라 overlay로 그린다.
        void Draw(GraphicsContext& context, const Scene& scene, const Math::Camera& camera, float width, float height)
        {
            // 등록 실패 시에는 화면 자료를 만들지 않는다.
            if (!Enabled || m_State.Bodies.size() != 3) return;
            GizmoDraw draw;
            draw.Reserve(48);
            const auto* object = scene.FindObject(m_Owner);
            if (!object) return;
            const auto* nodes = object->GetModelInstance()->GetNodeTransforms();

            // CollisionDebugDraw와 같은 view 변환·근평면 clipping·projection 순서를 쓴다.
            const auto line = [&](Math::Vector3 a, Math::Vector3 b, Color color)
            {
                // 원근 나눗셈 전에 view 공간 근평면으로 선분을 자른다.
                a = Math::MakeHomogeneous(camera.GetViewMatrix() * a);
                b = Math::MakeHomogeneous(camera.GetViewMatrix() * b);
                CollisionDebug::ViewPoint va{ float(a.GetX()), float(a.GetY()), float(a.GetZ()) };
                CollisionDebug::ViewPoint vb{ float(b.GetX()), float(b.GetY()), float(b.GetZ()) };
                if (!CollisionDebug::ClipSegmentToNearPlane(va, vb, camera.GetNearClip())) return;
                GizmoMath::Point pa, pb;
                if (GizmoMath::Project(Math::Vector3(va.X, va.Y, va.Z), camera.GetProjMatrix(), width, height, pa) &&
                    GizmoMath::Project(Math::Vector3(vb.X, vb.Y, vb.Z), camera.GetProjMatrix(), width, height, pb))
                    draw.Line(pa, pb, color, 2.0f);
            };

            // 월드 위치에 색상과 크기를 지정한 십자 표식을 만든다.
            const auto mark = [&](Math::Vector3 p, Color color, float size)
            {
                // 월드 공간 십자로 점의 깊이와 위치를 표현한다.
                line(p - Math::Vector3(size, 0, 0), p + Math::Vector3(size, 0, 0), color);
                line(p - Math::Vector3(0, size, 0), p + Math::Vector3(0, size, 0), color);
            };

            // 확정 본의 위치와 부모 연결을 청록색으로 그린다.
            for (uint32_t i = 0; i < (Xbot ? 3u : 4u); ++i)
            {
                const uint32_t node = Xbot ? 13 + i : i;
                const Math::Vector3 bone((m_ModelToWorld * nodes[node]).GetW());
                mark(bone, Color(0.0f, 1.0f, 1.0f), 0.035f);
                if (i > 0) line(Math::Vector3((m_ModelToWorld * nodes[node - 1]).GetW()), bone, Color(0.0f, 1.0f, 1.0f));
            }

            // 실제 body COM을 노란색 십자로 그린다.
            for (const auto& body : m_State.Bodies)
                mark(Math::Vector3(float(body.CenterOfMass[0]), float(body.CenterOfMass[1]), float(body.CenterOfMass[2])),
                    Color(1.0f, 1.0f, 0.0f), 0.07f);

            // 부모와 자식 anchor를 빨강과 초록으로 각각 그린다.
            for (const auto& joint : m_State.Joints)
            {
                mark(joint.ParentAnchor, Color(1, 0.2f, 0.2f), 0.06f);
                mark(joint.ChildAnchor, Color(0.2f, 1, 0.2f), 0.03f);
                line(joint.ParentAnchor, joint.ChildAnchor, Color(1, 0.5f, 0));
            }

            // 모은 선분을 overlay에 한 번 제출한다.
            draw.Render(context, width, height);
        }

    private:
        // 렌더에 전달하는 최종 node와 모든 해당 팔레트를 실제 body 및 독립 애니메이션과 비교한다.
        bool CheckRenderPose(const GameObject& object)
        {
            // 물리 기대값은 target이나 override 대신 진단으로 읽은 실제 body 원점에서 복원한다.
            const auto* pose = object.GetModelInstance();
            const auto model = pose->GetModel();
            bool passed = true;
            for (unsigned body = 1; body < 3; ++body)
            {
                const uint32_t node = m_State.Bodies[body].Node;
                Math::Matrix4 rigid, offset;
                if (!SkeletalFrames::TryRigidMatrix(m_State.Bodies[body].Frame, rigid) ||
                    !SkeletalFrames::TryRigidMatrix(m_XbotAsset.Bodies[body].BoneFromBody, offset)) return false;
                const Math::Vector3 physical((rigid * Math::Invert(offset) * Math::Matrix4::MakeScale(m_NodeScale[body])).GetW());
                unsigned entries = 0;

                // 각 스킨 section에서 렌더와 같은 E를 만들고 사용 중인 팔레트만 읽는다.
                for (const auto& component : object.GetMeshComponents())
                    for (size_t section = 0; section < component->GetGeometry()->GetSections().size(); ++section)
                    {
                        const auto& mesh = component->GetGeometry()->GetMesh(section);
                        const Math::Matrix4 placement = Math::Matrix4(component->GetWorldTransform(object.GetTransform())) *
                            Math::Matrix4(Math::Invert(component->GetDeformBinding().SourceFrame.ToAffine()));
                        const Math::Matrix4 e = placement * pose->GetNodeTransforms()[mesh.meshCBV];
                        const Math::Matrix4 animationE = placement * m_Animation->GetNodeTransforms()[mesh.meshCBV];
                        for (uint32_t j = mesh.startJoint; j < uint32_t(mesh.startJoint) + mesh.numJoints; ++j)
                        {
                            if (model->m_JointIndices[j] != node) continue;

                            // inverse bind를 풀어 팔레트의 본 원점을 구하며 별도 animation은 독립 E로 계산한다.
                            const Math::Vector3 palette((e * pose->GetJoints()[j].posXform * Math::Invert(model->m_JointIBMs[j])).GetW());
                            const Math::Vector3 finalNode((e * pose->GetNodeTransforms()[node]).GetW());
                            const Math::Vector3 animation((animationE * m_Animation->GetNodeTransforms()[node]).GetW());
                            const double paletteError = double(Math::Length(palette - physical));
                            const double nodeError = double(Math::Length(finalNode - physical));
                            const double animationDelta = double(Math::Length(palette - animation));
                            const double nodeAnimationDelta = double(Math::Length(finalNode - animation));
                            const bool valid = std::isfinite(paletteError) && std::isfinite(nodeError) &&
                                std::isfinite(animationDelta) && std::isfinite(nodeAnimationDelta) &&
                                paletteError <= 1e-3 && nodeError <= 1e-3 && animationDelta >= 0.1 && nodeAnimationDelta >= 0.1;
                            passed &= valid;
                            ++entries;
                            m_Log << "BC-12b render node=" << node << " palette=" << j << " scale=" << m_NodeScale[body]
                                << " paletteBodyError=" << paletteError << " nodeBodyError=" << nodeError
                                << " animationDelta=" << animationDelta << " nodeAnimationDelta=" << nodeAnimationDelta
                                << " " << (valid ? "PASS" : "FAIL") << "\n";
                        }
                    }
                passed &= entries > 0;
            }
            return passed;
        }

        // 모드별 로그 접두사를 반환한다.
        const char* Label() const
        {
            // 기존 fixture 로그 이름을 유지한다.
            return Xbot ? "BC-12b" : "BC-12a";
        }

        // 시작 장면 Xbot의 본 구조와 실제 렌더 frame으로 왼팔 rig를 등록한다.
        void StartXbot(Scene& scene, PhysicsSystem& physics)
        {
            // 기본 Xbot이 없거나 모델 구조가 다르면 정상 종료로 실패를 남긴다.
            GameObject* xbot = nullptr;
            for (const auto& object : scene.GetObjects())
                if (object->GetName() == "Xbot") xbot = object.get();
            std::vector<NodeId> parents;
            MiniPhysics::SkeletalPhysicsAsset asset;
            if (!xbot || !xbot->GetModelInstance())
            {
                Finish(false);
                return;
            }
            const auto model = xbot->GetModelInstance()->GetModel();
            if (!BuildPoseParents(model->m_SceneGraph.get(), model->m_NumNodes, parents) ||
                !MiniPhysics::CreateXbotLeftArmAsset(parents, asset) || !model->m_NumAnimations)
            {
                Finish(false);
                return;
            }

            // 독립 pose와 장면 pose를 같은 클립 시작점에 맞춘다.
            xbot->PlayAnimation(0, true);
            scene.InitializePoses();
            m_Animation = std::make_unique<ModelInstance>(model);
            m_Animation->PlayAnimation(0, true);
            m_Animation->Update(0);
            m_Owner = xbot->GetId();
            const auto* nodes = xbot->GetModelInstance()->GetNodeTransforms();
            bool found = false;

            // 렌더와 같은 E를 구하고 모든 스킨 section의 일치를 검사한다.
            for (const auto& component : xbot->GetMeshComponents())
                for (size_t section = 0; section < component->GetGeometry()->GetSections().size(); ++section)
                {
                    const auto& mesh = component->GetGeometry()->GetMesh(section);
                    if (!mesh.numJoints) continue;
                    const Math::Matrix4 e = Math::Matrix4(component->GetWorldTransform(xbot->GetTransform())) *
                        Math::Matrix4(Math::Invert(component->GetDeformBinding().SourceFrame.ToAffine())) * nodes[mesh.meshCBV];
                    DirectX::XMFLOAT4X4 delta;
                    DirectX::XMStoreFloat4x4(&delta, Math::Invert(e) * (found ? m_ModelToWorld : e));
                    for (unsigned row = 0; row < 4; ++row)
                        for (unsigned col = 0; col < 4; ++col)
                            m_Finite &= std::abs(delta.m[row][col] - (row == col ? 1.0f : 0.0f)) <= 1e-4f;
                    m_ModelToWorld = e;
                    found = true;
                }

            // 배율 분리와 등록은 기존 rig 경로를 재사용한다.
            auto& skeletal = physics.GetSkeletalPhysics();
            if (!found || !m_Finite || !physics.IsSimulationEnabled() ||
                !skeletal.RegisterFromPose(m_Owner, asset, parents, m_ModelToWorld,
                    std::vector<Math::Matrix4>(nodes, nodes + model->m_NumNodes)) ||
                !skeletal.ReadDiagnosticState(m_Owner, m_State))
            {
                Finish(false);
                return;
            }
            m_Initial = m_State;
            m_XbotAsset = asset;
            for (unsigned body = 0; body < 3; ++body)
                m_NodeScale[body] = float(Math::Length(Math::Vector3((m_ModelToWorld * nodes[asset.Bodies[body].Node]).GetX())));
            m_Log << "BC-12b Xbot nodes=70 bodies=3 joints=2 fixedDt=1/60 unbroken=2 enabled=0\n";
        }

        // PASS/FAIL을 파일과 debugger 양쪽에 남기고 정상 종료를 요청한다.
        void Finish(bool passed)
        {
            // 수치 로그를 먼저 flush하여 종료 뒤에도 읽을 수 있게 한다.
            m_Log << Label() << " " << (passed ? "PASS" : "FAIL") << "\n";
            m_Log.flush();
            Utility::Printf("%s %s: ticks=%u finite=%u wristMaxError=%.9g\n",
                Label(), passed ? "PASS" : "FAIL", m_Ticks, m_Finite ? 1u : 0u, m_MaxAnchorError);
            Done = true;
        }
        ObjectId m_Owner = InvalidObjectId;
        unsigned m_Ticks = 0;
        unsigned m_MaxErrorTick = 0;
        unsigned m_ContactTick = 0;
        double m_PreviousLowerVelocityY = 0;
        double m_FreeFallMaxError = 0;
        double m_ContactMaxError = 0;
        bool m_Finite = true;
        double m_MaxAnchorError = 0;
        SkeletalRigState m_Initial, m_State;
        std::ofstream m_Log;
        double m_AnimationError = 0;
        double m_BreakChildY = 0;
        std::unique_ptr<ModelInstance> m_Animation;
        Math::Matrix4 m_ModelToWorld{ Math::kIdentity };
        MiniPhysics::SkeletalPhysicsAsset m_XbotAsset;
        float m_NodeScale[3]{};
    };
}
