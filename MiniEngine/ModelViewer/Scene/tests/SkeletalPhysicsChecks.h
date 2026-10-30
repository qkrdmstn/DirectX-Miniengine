#pragma once

#include "../PhysicsSystem.h"
#include "../CollisionDebugMath.h"
#include "../GizmoDraw.h"
#include "Camera.h"
#include "Utility.h"
#include <fstream>

namespace Viewer
{
    // 기존 모델과 독립된 세 상자 rig를 공통 프레임 경로로 검사한다.
    class SkeletalPhysicsChecks
    {
    public:
        bool Enabled = false;
        bool Done = false;

        // 메시 없는 본 모델을 추가하고 초기 실제 body 상태를 저장한다.
        void Start(Scene& scene, PhysicsSystem& physics)
        {
            // 원본 모델은 건드리지 않고 정지 local pose 네 개를 만든다.
            if (!Enabled) return;
            m_Log.open("SkeletalPhysicsChecks.log", std::ios::trunc);
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
            physics.QueueBreak({ m_Owner, 0, skeletal.GetRigGeneration(m_Owner),
                skeletal.GetTopologyGeneration(m_Owner) });
            m_Log << "BC-12a break queued at tick=60 impulse=0\n";
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
            for (uint32_t i = 0; i < 4; ++i)
            {
                const Math::Matrix4& matrix = pose->GetNodeTransforms()[i];
                const float* values = reinterpret_cast<const float*>(&matrix);
                for (unsigned j = 0; j < 16; ++j) m_Finite &= std::isfinite(values[j]);
            }
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
            m_Log << "BC-12a sample tick=" << m_Ticks << " error=" << error
                << " childComY=" << m_State.Bodies[1].CenterOfMass[1]
                << " lowerComY=" << m_State.Bodies[2].CenterOfMass[1]
                << " lowerVelocityY=" << lowerVelocityY << " contactTick=" << m_ContactTick << "\n";
            m_Finite &= std::isfinite(error);
            if (m_ContactTick == 0) m_FreeFallMaxError = std::max(m_FreeFallMaxError, error);
            else m_ContactMaxError = std::max(m_ContactMaxError, error);

            // 마지막 tick에서 root 유지·자손 낙하와 구간별 손목 오차를 판정한다.
            if (m_Ticks != 180) return;
            const double rootDelta = m_State.Bodies[0].CenterOfMass[1] - m_Initial.Bodies[0].CenterOfMass[1];
            const double childDelta = m_State.Bodies[1].CenterOfMass[1] - m_Initial.Bodies[1].CenterOfMass[1];
            const bool topology = m_State.Joints[0].Broken && !m_State.Joints[1].Broken &&
                m_State.Joints[1].Solver.Enabled && m_State.Joints[1].Solver.MotorsOff;
            m_Log << "BC-12a ticks=180 rootDeltaY=" << rootDelta << " childDeltaY=" << childDelta
                << " wristMaxError=" << m_MaxAnchorError << " maxErrorTick=" << m_MaxErrorTick
                << " contactTick=" << m_ContactTick << " freeFallMaxError=" << m_FreeFallMaxError
                << " contactMaxError=" << m_ContactMaxError << " finalError=" << error
                << " finite=" << m_Finite << " topology=" << topology << "\n";
            Finish(std::abs(rootDelta) <= 1e-5 && childDelta < -0.01 && m_ContactTick > 60 &&
                m_FreeFallMaxError <= 0.01 && m_ContactMaxError <= 0.02 && error <= 0.01 && m_Finite && topology);
        }

        // 실제 COM·본·양쪽 anchor를 카메라 근평면으로 잘라 overlay로 그린다.
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
            for (uint32_t i = 0; i < 4; ++i)
            {
                const Math::Vector3 bone(nodes[i].GetW());
                mark(bone, Color(0.0f, 1.0f, 1.0f), 0.035f);
                if (i > 0) line(Math::Vector3(nodes[i - 1].GetW()), bone, Color(0.0f, 1.0f, 1.0f));
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
        // PASS/FAIL을 파일과 debugger 양쪽에 남기고 정상 종료를 요청한다.
        void Finish(bool passed)
        {
            // 수치 로그를 먼저 flush하여 종료 뒤에도 읽을 수 있게 한다.
            m_Log << "BC-12a " << (passed ? "PASS" : "FAIL") << "\n";
            m_Log.flush();
            Utility::Printf("BC-12a %s: ticks=%u finite=%u wristMaxError=%.9g\n",
                passed ? "PASS" : "FAIL", m_Ticks, m_Finite ? 1u : 0u, m_MaxAnchorError);
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
    };
}
