// VS Developer PowerShell, 저장소 루트: CPU 산출물은 TEMP/BreakConstraintTests에만 둔다.
// $bc = Join-Path $env:TEMP 'BreakConstraintTests'; New-Item -ItemType Directory -Force $bc | Out-Null
// $sdk = 'MiniEngine/Build/ThirdParty/Jolt/build'
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /MDd /DJPH_OBJECT_LAYER_BITS=16 /I MiniEngine/Physics /I MiniEngine/Core /I "$sdk/_deps/joltsource-src" "/Fo:$bc/" "/Fe:$bc/SkeletalBreakTests.exe" MiniEngine/ModelViewer/Scene/tests/SkeletalBreakTests.cpp MiniEngine/ModelViewer/Scene/SkeletalPhysicsSystem.cpp MiniEngine/Physics/PhysicsWorld.cpp MiniEngine/Physics/PhysicsWorldBodies.cpp MiniEngine/Physics/PhysicsWorldBreak.cpp MiniEngine/Physics/PhysicsWorldConstraints.cpp MiniEngine/Physics/PhysicsWorldStep.cpp MiniEngine/Physics/Collision/ConvexElem.cpp MiniEngine/Physics/Collision/AggregateGeom.cpp MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.cpp "$sdk/lib/Debug/Jolt.lib" /link "/PDB:$bc/SkeletalBreakTests.pdb"
// & "$bc/SkeletalBreakTests.exe"
// Release는 /MDd 대신 /MD /O2 /DNDEBUG, lib/Release/Jolt.lib를 사용한다.
#include "../../../Physics/tests/CollisionTestUtils.h"
#include "../../../Physics/PhysicsWorldInternal.h"
#include "../SkeletalPhysicsSystem.h"
#include "../SkeletalPhysicsFrames.h"
#include "../../../Physics/Collision/AggregateGeom.h"
#include <cstdio>

using namespace MiniPhysics;
using Viewer::SkeletalPhysicsSystem;

// world의 body·관절·pair 제외·용량 예약이 모두 0인지 검사한다.
static void CheckEmpty(PhysicsWorld& world, const char* message)
{
    // 공개 수와 내부 pair 필터를 함께 읽는다.
    const auto* impl = PhysicsWorldAccess::GetImpl(world);
    Check(world.GetBodyCount() == 0 && world.GetConstraintCount() == 0 && world.GetReservedCount() == 0 &&
        impl->PairFilter->Pairs.empty() && world.GetExcludedPairCount() == 0, message);
}

// BoneFromBody가 identity인 fixture는 body 원점 frame이 해당 본의 rest frame과 같다.
static std::vector<SkeletalRigidFrame> FixtureBodyFrames(const SkeletalPhysicsFixture& fixture)
{
    // body ID 순서로 대응 본의 frame을 모은다.
    std::vector<SkeletalRigidFrame> frames;
    for (const auto& body : fixture.Asset.Bodies) frames.push_back(fixture.RestNodeFrames[body.Node]);
    return frames;
}

// 진단 조회가 실제 step 결과를 읽고 실패 시 이전 묶음을 보존하는지 검사한다.
static void CheckDiagnosticState(PhysicsWorld& world, SkeletalPhysicsSystem& skeletal)
{
    // 정지 fixture에서 body 세 개와 disabled 관절 두 개를 읽는다.
    const auto fixture = CreateThreeBodyFixture();
    Check(skeletal.Register(20, fixture.Asset, fixture.NodeParents, FixtureBodyFrames(fixture)), "Diagnostic register");
    Viewer::SkeletalRigState state;
    Check(skeletal.ReadDiagnosticState(20, state), "Diagnostic read rest");
    Check(state.Bodies.size() == 3 && state.Joints.size() == 2, "Diagnostic counts");
    for (size_t i = 0; i < state.Bodies.size(); ++i)
    {
        const auto& body = state.Bodies[i];
        Check(body.Node == i && body.Motion == BodyMotion::Kinematic, "Diagnostic node and motion");
        for (size_t axis = 0; axis < 3; ++axis)
        {
            CheckNear(body.Frame.Position[axis], fixture.RestNodeFrames[i].Position[axis], 1e-6, "Diagnostic rest pose");
            CheckNear(body.CenterOfMass[axis], body.Frame.Position[axis], 1e-6, "Diagnostic rest COM");
            CheckNear(body.Linear[axis], 0, 1e-6, "Diagnostic rest linear velocity");
            CheckNear(body.Angular[axis], 0, 1e-6, "Diagnostic rest angular velocity");
        }
    }
    for (size_t i = 0; i < state.Joints.size(); ++i)
    {
        const auto& joint = state.Joints[i];
        Check(!joint.Broken && !joint.Solver.Enabled && joint.Solver.MotorsOff, "Diagnostic resting joint");
        CheckNear(float(joint.ParentAnchor.GetY()), 1.75 - 0.5 * i, 1e-6, "Diagnostic world anchor");
        Check(float(Math::Length(joint.ParentAnchor - joint.ChildAnchor)) < 1e-6, "Diagnostic anchor coincidence");
    }

    // 목표와 다른 실제 원점 pose를 주면 회전된 body-local anchor도 월드로 변환해야 한다.
    const auto root = skeletal.GetBodyHandle(20, 0);
    Check(world.SetBodyTransform(root, { 3, 4, 5 }, { 0, 0, std::sqrt(0.5), std::sqrt(0.5) }),
        "Diagnostic rotated actual pose");
    Check(skeletal.ReadDiagnosticState(20, state), "Diagnostic read actual instead of target");
    Check(state.Bodies[0].Frame.Position == std::array<double, 3>{ 3, 4, 5 }, "Diagnostic actual position");
    CheckNear(float(state.Joints[0].ParentAnchor.GetX()), 3.25, 1e-6, "Diagnostic rotated anchor X");
    CheckNear(float(state.Joints[0].ParentAnchor.GetY()), 4, 1e-6, "Diagnostic rotated anchor Y");
    CheckNear(float(state.Joints[0].ParentAnchor.GetZ()), 5, 1e-6, "Diagnostic translated anchor Z");
    Check(world.SetBodyTransform(root, { 0, 2, 0 }, { 0, 0, 0, 1 }), "Diagnostic restore root");

    // 실제 분리와 120 step 뒤의 COM 하강 및 살아 있는 손목 오차를 확인한다.
    Check(skeletal.BreakConstraint({ 20, 0, skeletal.GetRigGeneration(20), 1 }) == Viewer::BreakStatus::Applied,
        "Diagnostic break");
    for (unsigned tick = 0; tick < 120; ++tick)
    {
        Check(world.Step(1.0f / 60), "Diagnostic world step");
        Check(skeletal.ReadDiagnosticState(20, state), "Diagnostic finite step state");
        Check(float(Math::Length(state.Joints[1].ParentAnchor - state.Joints[1].ChildAnchor)) <= 0.01f,
            "Diagnostic wrist anchor tolerance");
    }
    CheckNear(state.Bodies[0].CenterOfMass[1], 2, 1e-6, "Diagnostic root stays");
    Check(state.Bodies[1].CenterOfMass[1] < 1.5 && state.Bodies[2].CenterOfMass[1] < 1,
        "Diagnostic detached COM falls");
    Check(state.Bodies[1].Motion == BodyMotion::Dynamic && state.Bodies[2].Motion == BodyMotion::Dynamic,
        "Diagnostic detached motion");
    Check(state.Joints[0].Broken && !state.Joints[0].Solver.Enabled && state.Joints[1].Solver.Enabled &&
        state.Joints[1].Solver.MotorsOff, "Diagnostic broken and enabled separated");

    // 미등록 owner와 마지막 body 또는 살아 있는 관절의 stale handle은 부분 결과도 게시하지 않는다.
    const double previousY = state.Bodies[1].CenterOfMass[1];
    Check(!skeletal.ReadDiagnosticState(999, state), "Diagnostic unknown owner rejected");
    Check(world.RemoveConstraint(skeletal.GetConstraintHandle(20, 1)), "Diagnostic stale joint setup");
    Check(!skeletal.ReadDiagnosticState(20, state), "Diagnostic stale joint rejected");
    Check(world.DestroyBody(skeletal.GetBodyHandle(20, 2)), "Diagnostic stale last body setup");
    Check(!skeletal.ReadDiagnosticState(20, state), "Diagnostic stale last body rejected");
    Check(state.Bodies.size() == 3 && state.Joints.size() == 2 &&
        state.Bodies[1].CenterOfMass[1] == previousY && state.Joints[1].Solver.Enabled,
        "Diagnostic failure preserves output");
    skeletal.Clear();
    CheckEmpty(world, "Diagnostic cleanup");
}

// 본 pose 등록·전체 목표 선검증·COM 속도·Dynamic 보존을 검사한다.
static void CheckPoseTargets(PhysicsWorld& world, SkeletalPhysicsSystem& skeletal)
{
    // Xbot 배율과 회전 body offset을 가진 fixture를 본 pose에서 등록한다.
    using namespace Math;
    auto fixture = CreateThreeBodyFixture();
    fixture.Asset.Bodies[0].BoneFromBody.Position = { 0.2, 0, 0 };
    std::vector<Matrix4> nodes;
    for (const auto& rest : fixture.RestNodeFrames)
    {
        Matrix4 pose;
        Check(Viewer::SkeletalFrames::TryRigidMatrix(rest, pose), "Fixture node matrix");
        nodes.push_back(pose * Matrix4::MakeScale(0.0177327f));
    }
    const Matrix4 identity(kIdentity);
    Check(skeletal.RegisterFromPose(10, fixture.Asset, fixture.NodeParents, identity, nodes), "Register scaled pose");
    Viewer::SkeletalBodyTarget target;
    Check(skeletal.GetBodyTarget(10, 0, target), "Read initial target");
    CheckNear(target.Scale, 0.0177327, 1e-8, "Registration scale saved");
    CheckNear(target.Frame.Position[0], 0.2, 1e-6, "Offset remains world meters");
    Check(target.Linear == std::array<double, 3>{} && target.Angular == std::array<double, 3>{}, "Registration velocity zero");

    // body 원점과 다른 COM을 가진 shape로 회전 이동 속도를 확인한다.
    const auto first = skeletal.GetBodyHandle(10, 0);
    MiniCollision::AggregateGeom geometry;
    std::vector<MiniCollision::Vec3d> corners;
    for (int i = 0; i < 8; ++i)
        corners.push_back({ 0.2 + ((i & 1) ? 0.1 : -0.1), (i & 2) ? 0.25 : -0.25, (i & 4) ? 0.1 : -0.1 });
    Check(MiniCollision::GenerateKDop18(corners, geometry), "COM fixture geometry");
    Check(world.SetBodyShape(first, geometry, { 1, 1, 1 }, 1), "Install COM offset shape");
    const float dt = 1.0f / 60;
    const Matrix4 moved(OrthogonalTransform(Quaternion(Vector3(0, 0, 1), 0.01f), Vector3(0.01f, 0, 0)));
    Check(skeletal.UpdateKinematicTargets(10, moved, nodes, dt), "Move kinematic targets");
    Check(skeletal.GetBodyTarget(10, 0, target), "Read moved target");
    CheckNear(target.Angular[2], 0.6, 1e-4, "World angular velocity");
    Check(std::abs(target.Linear[1] - (target.Frame.Position[1] - 2.0) / dt) > 0.1, "COM velocity differs from origin velocity");
    Check(world.Step(dt), "Step to target");
    std::array<double, 3> position{}, linear{}, angular{};
    std::array<double, 4> rotation{};
    Check(world.GetBodyPose(first, position, rotation), "Read target body pose");
    for (int i = 0; i < 3; ++i) CheckNear(position[i], target.Frame.Position[i], 1e-5, "Body origin reaches target");

    // 마지막 본의 오류와 배율·시간 오류는 앞 body 속도를 바꾸지 않는다.
    Check(world.GetBodyVelocity(first, linear, angular), "Snapshot velocity");
    auto bad = nodes;
    bad[2] = Matrix4::MakeScale(Vector3(1, 2, 1));
    Check(!skeletal.UpdateKinematicTargets(10, identity, bad, dt), "Late invalid target rejected");
    Check(!skeletal.UpdateKinematicTargets(10, Matrix4::MakeScale(2), nodes, dt), "Owner scale change rejected");
    Check(!skeletal.UpdateKinematicTargets(10, moved, nodes, 0), "Zero dt rejected");
    Check(!skeletal.UpdateKinematicTargets(10, moved, nodes, 1e-30f), "Tiny dt rejected");
    std::array<double, 3> currentLinear{}, currentAngular{};
    Check(world.GetBodyVelocity(first, currentLinear, currentAngular) && linear == currentLinear && angular == currentAngular, "Failed batch leaves velocity unchanged");

    // 이미 Dynamic인 body의 pose와 속도는 보존한다.
    Check(world.SetBodyMotion(first, BodyMotion::Dynamic), "Convert one body dynamic");
    Check(world.SetBodyVelocity(first, { 1, 2, 3 }, { 0.1, 0.2, 0.3 }), "Dynamic velocity set");
    Check(world.GetBodyPose(first, position, rotation), "Snapshot dynamic pose");
    Check(world.GetBodyVelocity(first, linear, angular), "Snapshot dynamic velocity");
    Check(skeletal.UpdateKinematicTargets(10, identity, nodes, dt), "Mixed motion update");
    auto slowlyScaled = nodes;
    for (auto& node : slowlyScaled) node = node * Matrix4::MakeScale(1.00005f);
    Check(skeletal.UpdateKinematicTargets(10, identity, slowlyScaled, dt), "Scale roundoff tolerated");
    for (auto& node : slowlyScaled) node = node * Matrix4::MakeScale(1.00005f);
    for (auto& node : slowlyScaled) node = node * Matrix4::MakeScale(1.00005f);
    Check(!skeletal.UpdateKinematicTargets(10, identity, slowlyScaled, dt), "Scale tolerance cannot accumulate");
    std::array<double, 3> currentPosition{};
    std::array<double, 4> currentRotation{};
    Check(world.GetBodyPose(first, currentPosition, currentRotation) && currentPosition == position && currentRotation == rotation, "Dynamic pose preserved");
    Check(world.GetBodyVelocity(first, currentLinear, currentAngular) && currentLinear == linear && currentAngular == angular, "Dynamic velocity preserved");

    // 제거된 마지막 body도 전체 목표를 적용하기 전에 거절한다.
    Check(world.DestroyBody(skeletal.GetBodyHandle(10, 2)), "Remove late body");
    Check(!skeletal.UpdateKinematicTargets(10, moved, nodes, dt), "Stale late body rejected");
    Check(skeletal.Unregister(10), "Pose rig released");
    CheckEmpty(world, "Pose tests leave no resources");
}

// 두 행렬의 모든 성분을 비교해 회전·위치·배율의 오차를 함께 검사한다.
static void CheckPoseMatrix(const Math::Matrix4& actual, const Math::Matrix4& expected)
{
    // SIMD 저장 순서가 같으므로 각 성분의 절대 오차를 대조한다.
    DirectX::XMFLOAT4X4 a, b;
    DirectX::XMStoreFloat4x4(&a, actual);
    DirectX::XMStoreFloat4x4(&b, expected);
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned column = 0; column < 4; ++column)
            CheckNear(a.m[row][column], b.m[row][column], 2e-4, "Physics node matrix component");
}

// 물리 본 world 고정·부모 이동 상쇄·비물리 tip 상속·등록 배율 복원을 검사한다.
static void CheckPoseWriteback(PhysicsWorld& world, SkeletalPhysicsSystem& skeletal)
{
    // 본 배율과 body offset이 있는 4-node fixture의 원본 local 애니메이션을 만든다.
    using namespace Math;
    auto fixture = CreateThreeBodyFixture();
    fixture.Asset.Bodies[1].BoneFromBody.Position = { 0.1, -0.05, 0 };
    const float scale = 0.0177327f;
    std::vector<Matrix4> nodes;
    GraphNode graph[4]{};
    for (uint32_t i = 0; i < 4; ++i)
    {
        Matrix4 rest;
        Check(Viewer::SkeletalFrames::TryRigidMatrix(fixture.RestNodeFrames[i], rest), "Writeback rest matrix");
        nodes.push_back(rest * Matrix4::MakeScale(scale));
        graph[i].xform = i == 0 ? nodes[i] : Invert(nodes[i - 1]) * nodes[i];
        graph[i].matrixIdx = i;
        graph[i].hasChildren = i < 3;
    }
    const Matrix4 identity(kIdentity);
    Check(skeletal.RegisterFromPose(20, fixture.Asset, fixture.NodeParents, identity, nodes), "Writeback rig registered");
    std::vector<NodePhysicsPose> overrides{ { 99, identity } };
    Check(skeletal.CollectPoseOverrides(20, identity, overrides) && overrides.empty(), "Kinematic rig has no overrides");

    // 두 자손 body를 물리 제어로 전환하고 애니메이션과 다른 world pose를 설정한다.
    const auto child = skeletal.GetBodyHandle(20, 1);
    const auto hand = skeletal.GetBodyHandle(20, 2);
    Check(world.SetBodyMotion(child, BodyMotion::Dynamic) && world.SetBodyMotion(hand, BodyMotion::Dynamic), "Writeback bodies dynamic");
    const SkeletalRigidFrame childFrame{ { 1, 1.25, -0.5 }, { 0, 0, std::sin(0.1), std::cos(0.1) } };
    const SkeletalRigidFrame handFrame{ { 1.2, 0.8, -0.4 }, { std::sin(0.05), 0, 0, std::cos(0.05) } };
    Check(world.SetBodyTransform(child, childFrame.Position, childFrame.Rotation), "Child world pose set");
    Check(world.SetBodyTransform(hand, handFrame.Position, handFrame.Rotation), "Hand world pose set");
    Check(skeletal.CollectPoseOverrides(20, identity, overrides) && overrides.size() == 2 &&
        overrides[0].Node == 1 && overrides[1].Node == 2, "Only dynamic node ids collected");

    // owner 회전·이동과 부모 애니메이션 변화에도 두 물리 본은 기존 world pose를 유지한다.
    const Matrix4 moved(OrthogonalTransform(Quaternion(Vector3(0, 1, 0), 0.35f), Vector3(3, 2, -1)));
    Check(skeletal.CollectPoseOverrides(20, moved, overrides), "Moved owner overrides collected");
    graph[0].xform = Matrix4(Matrix3(kIdentity), Vector3(0, 0.4f, 0)) * graph[0].xform;
    graph[3].xform = graph[3].xform * Matrix4(Matrix3(kIdentity), Vector3(0.2f, 0, 0));
    Matrix4 result[4];
    Check(EvaluatePoseOverrides(graph, 4, fixture.NodeParents, overrides.data(), uint32_t(overrides.size()), result), "Overrides finalized with local animation");
    Matrix4 childWorld, handWorld, offset;
    Check(Viewer::SkeletalFrames::TryRigidMatrix(childFrame, childWorld), "Expected child world");
    Check(Viewer::SkeletalFrames::TryRigidMatrix(handFrame, handWorld), "Expected hand world");
    Check(Viewer::SkeletalFrames::TryRigidMatrix(fixture.Asset.Bodies[1].BoneFromBody, offset), "Expected body offset");
    CheckPoseMatrix(moved * result[1], childWorld * Invert(offset) * Matrix4::MakeScale(scale));
    CheckPoseMatrix(moved * result[2], handWorld * Matrix4::MakeScale(scale));
    CheckPoseMatrix(result[0], graph[0].xform);
    CheckPoseMatrix(result[3], result[2] * graph[3].xform);
    SkeletalRigidFrame restored;
    float restoredScale = 0;
    Check(Viewer::SkeletalFrames::TrySplitUniformFrame(moved * result[2], restored, restoredScale), "Restored node uniform frame");
    CheckNear(restoredScale, scale, 1e-8, "Registered node scale restored");

    // 무효 frame·미등록 owner·마지막 stale body는 수집된 앞 항목도 게시하지 않는다.
    const auto saved = overrides;
    Check(!skeletal.CollectPoseOverrides(999, moved, overrides), "Unregistered writeback rejected");
    Check(!skeletal.CollectPoseOverrides(20, Matrix4::MakeScale(2), overrides), "Writeback owner scale change rejected");
    Check(!skeletal.CollectPoseOverrides(20, Matrix4::MakeScale(Vector3(1, 2, 1)), overrides), "Writeback nonuniform E rejected");
    Check(!skeletal.CollectPoseOverrides(20, Matrix4::MakeScale(0), overrides), "Writeback singular E rejected");
    Check(world.DestroyBody(hand), "Remove late writeback body");
    Check(!skeletal.CollectPoseOverrides(20, moved, overrides), "Stale late writeback rejected");
    Check(overrides.size() == saved.size(), "Writeback failure keeps output size");
    for (size_t i = 0; i < saved.size(); ++i)
    {
        Check(overrides[i].Node == saved[i].Node, "Writeback failure keeps node ids");
        CheckPoseMatrix(overrides[i].ModelPose, saved[i].ModelPose);
    }

    // 수집은 pose·자원 소유권을 바꾸지 않으며 기존 해제 순서로 모두 정리한다.
    Check(skeletal.Unregister(20), "Writeback rig released");
    CheckEmpty(world, "Writeback tests leave no resources");
}

// 동기 break의 원자적 거절·속도 상속·반복 요청·끊긴 경계 탐색을 검사한다.
static void CheckSynchronousBreak(PhysicsWorld& world, SkeletalPhysicsSystem& skeletal)
{
    // rest fixture와 세대가 일치하는 요청을 준비한다.
    using Viewer::BreakStatus;
    const auto fixture = CreateThreeBodyFixture();
    const auto frames = FixtureBodyFrames(fixture);
    Check(skeletal.Register(30, fixture.Asset, fixture.NodeParents, frames), "Break fixture registered");
    const auto root = skeletal.GetBodyHandle(30, 0);
    const auto arm = skeletal.GetBodyHandle(30, 1);
    const auto hand = skeletal.GetBodyHandle(30, 2);
    const auto wrist = skeletal.GetConstraintHandle(30, 1);
    Viewer::BreakRequest request{ 30, 0, skeletal.GetRigGeneration(30), skeletal.GetTopologyGeneration(30) };
    request.WorldImpulse = { 1, 0, 0 };
    request.WorldPoint = { 0, 1.6f, 0 };
    auto bad = request;
    bad.ExpectedTopologyGeneration++;
    Check(skeletal.BreakConstraint(bad) == BreakStatus::Stale, "Stale topology rejected");
    bad = request;
    bad.ExpectedRigGeneration++;
    Check(skeletal.BreakConstraint(bad) == BreakStatus::Stale, "Stale rig rejected");
    bad = request;
    bad.WorldImpulse[0] = NAN;
    Check(skeletal.BreakConstraint(bad) == BreakStatus::InvalidInput, "NaN impulse rejected");
    bad = request;
    bad.WorldImpulse[0] = FLT_MAX;
    Check(skeletal.BreakConstraint(bad) == BreakStatus::InvalidInput, "Overflow impulse rejected");
    Check(world.GetConstraintCount() == 2 && skeletal.GetTopologyGeneration(30) == 1, "Rejected break leaves topology");
    BodyMotion motion;
    Check(world.GetBodyMotion(arm, motion) && motion == BodyMotion::Kinematic, "Rejected break leaves motion");

    // 목표를 아직 step하지 않아도 목표 pose로 전환하며 COM 속도를 상속한다.
    std::vector<Math::Matrix4> nodes;
    for (auto frame : fixture.RestNodeFrames)
    {
        frame.Position[0] += 0.01;
        Math::Matrix4 matrix;
        Check(Viewer::SkeletalFrames::TryRigidMatrix(frame, matrix), "Break target matrix");
        nodes.push_back(matrix);
    }
    Check(skeletal.UpdateKinematicTargets(30, Math::Matrix4(Math::kIdentity), nodes, 1.0f / 60), "Break targets before step");
    Check(skeletal.BreakConstraint(request) == BreakStatus::Applied, "Elbow break applied");
    Check(world.GetBodyCount() == 3 && world.GetConstraintCount() == 1, "Break preserves bodies and wrist");
    Check(skeletal.GetTopologyGeneration(30) == 2 && skeletal.GetConstraintHandle(30, 0) == InvalidConstraint, "Break publishes topology");
    Check(world.GetBodyMotion(root, motion) && motion == BodyMotion::Kinematic, "Root stays animation");
    Check(world.GetBodyMotion(arm, motion) && motion == BodyMotion::Dynamic, "Arm becomes physics");
    Check(world.GetBodyMotion(hand, motion) && motion == BodyMotion::Dynamic, "Hand becomes physics");
    ConstraintState state;
    Check(world.GetConstraintState(wrist, state) && state.Enabled && state.MotorsOff, "Wrist enabled motors off");
    std::array<double, 3> p{}, v{}, w{}, savedV{}, savedW{};
    std::array<double, 4> q{};
    Check(world.GetBodyPose(arm, p, q), "Read synchronized arm");
    CheckNear(p[0], 0.01, 1e-6, "Arm uses unstepped target");
    Check(world.GetBodyVelocity(arm, v, w), "Read impulse velocity");
    CheckNear(v[0], 1.6, 1e-4, "Inherited COM velocity plus impulse once");
    Check(std::abs(w[2]) > 0.01, "Off center impulse adds angular velocity");
    savedV = v;
    savedW = w;
    Check(skeletal.BreakConstraint(request) == BreakStatus::AlreadyBroken, "Duplicate with old topology already broken");
    Check(world.GetBodyVelocity(arm, v, w) && v == savedV && w == savedW, "Duplicate adds no impulse");
    Check(skeletal.GetTopologyGeneration(30) == 2, "Duplicate leaves generation");
    Check(PhysicsWorldAccess::GetImpl(world)->PairFilter->Pairs.size() == 2, "Broken adjacent pair still excluded");

    // 실제 낙하 후에도 남은 손목의 두 anchor가 붙어 있는지 검사한다.
    for (int tick = 0; tick < 120; ++tick) Check(world.Step(1.0f / 60), "Broken chain step");
    const auto* impl = PhysicsWorldAccess::GetImpl(world);
    const auto& joint = impl->Constraints.at(wrist).Joint;
    const auto& bodies = impl->World.GetBodyInterface();
    const auto a = bodies.GetCenterOfMassTransform(impl->SimBodies.at(arm)) * joint->GetLocalSpacePosition1();
    const auto b = bodies.GetCenterOfMassTransform(impl->SimBodies.at(hand)) * joint->GetLocalSpacePosition2();
    Check(JPH::Vec3(a - b).Length() <= 0.01f, "Wrist anchor stays connected after fall");

    // 이미 Dynamic인 자손은 재전환하거나 목표 pose로 되돌리지 않는다.
    request.JointId = 1;
    Check(skeletal.BreakConstraint(request) == BreakStatus::Stale, "Other joint with old generation stale");
    request.ExpectedTopologyGeneration = 2;
    request.WorldImpulse = {};
    Check(world.SetBodyVelocity(hand, { 0.2, 0.3, 0.4 }, { 0.1, 0.2, 0.3 }), "Set existing dynamic velocity");
    Check(world.GetBodyVelocity(hand, savedV, savedW), "Snapshot dynamic velocity");
    Check(skeletal.BreakConstraint(request) == BreakStatus::Applied, "Wrist breaks after elbow");
    Check(world.GetBodyVelocity(hand, v, w) && v == savedV && w == savedW, "Existing dynamic velocity preserved");
    Check(skeletal.Unregister(30), "Broken rig released");
    CheckEmpty(world, "Broken rig resources zero");

    // anchor 위반은 제한 검사 전에 거절하고 상태를 보존한다.
    auto badFrames = frames;
    badFrames[2].Position[0] += 0.01;
    Check(skeletal.Register(31, fixture.Asset, fixture.NodeParents, badFrames), "Misaligned fixture registered");
    request = { 31, 0, skeletal.GetRigGeneration(31), 1 };
    Check(skeletal.BreakConstraint(request) == BreakStatus::UnsupportedPose, "Bad internal anchor rejected");
    Check(world.GetConstraintCount() == 2 && skeletal.GetTopologyGeneration(31) == 1, "Bad pose no partial commit");
    Check(skeletal.Unregister(31), "Bad pose fixture released");

    // +X twist와 +Y swing을 각각 제한 밖과 안에서 검사한다.
    for (int pose = 0; pose < 4; ++pose)
    {
        const bool twist = pose % 2 == 0;
        const bool outside = pose < 2;
        const double angle = outside ? 1.2 : 0.4;
        const auto rotation = JPH::Quat::sRotation(twist ? JPH::Vec3::sAxisX() : JPH::Vec3::sAxisY(), float(angle));
        badFrames = frames;
        badFrames[2].Rotation = { rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW() };
        const auto rotatedAnchor = rotation * JPH::Vec3(0, 0.25f, 0);
        for (int axis = 0; axis < 3; ++axis)
            badFrames[2].Position[axis] = frames[1].Position[axis] + (axis == 1 ? -0.25 : 0) - rotatedAnchor[axis];
        Check(skeletal.Register(32, fixture.Asset, fixture.NodeParents, badFrames), "Limit pose fixture registered");
        request = { 32, 0, skeletal.GetRigGeneration(32), 1 };
        const auto limitWrist = skeletal.GetConstraintHandle(32, 1);
        const auto* limitImpl = PhysicsWorldAccess::GetImpl(world);
        const auto& limitJoint = limitImpl->Constraints.at(limitWrist).Joint;
        const auto& limitBodies = limitImpl->World.GetBodyInterface();

        // 실제 COM anchor 일치와 순수 회전축을 확인해 anchor 선거절을 배제한다.
        const auto parentTransform = limitBodies.GetCenterOfMassTransform(limitImpl->SimBodies.at(skeletal.GetBodyHandle(32, 1)));
        const auto childTransform = limitBodies.GetCenterOfMassTransform(limitImpl->SimBodies.at(skeletal.GetBodyHandle(32, 2)));
        const auto parentAnchor = parentTransform * limitJoint->GetLocalSpacePosition1();
        const auto childAnchor = childTransform * limitJoint->GetLocalSpacePosition2();
        Check(JPH::Vec3(parentAnchor - childAnchor).LengthSq() <= 1e-8f, "Limit pose passes anchor precondition");
        const auto relative = ((parentTransform.GetQuaternion() * limitJoint->GetConstraintToBody1()).Conjugated() *
            (childTransform.GetQuaternion() * limitJoint->GetConstraintToBody2())).Normalized();
        JPH::Quat poseSwing, poseTwist;
        relative.GetSwingTwist(poseSwing, poseTwist);
        Check((twist ? poseSwing : poseTwist).IsClose(JPH::Quat::sIdentity(), 1e-12f), "Limit pose isolates requested axis");
        const double limit = twist ? limitJoint->GetTwistMaxAngle() : limitJoint->GetNormalHalfConeAngle();
        Check(outside ? angle > limit : angle < limit, "Pose angle straddles configured limit");

        // 거절 전 모든 body의 pose와 속도 및 내부 관절 상태를 저장한다.
        std::array<std::array<double, 3>, 3> positions{}, velocities{}, angularVelocities{};
        std::array<std::array<double, 4>, 3> rotations{};
        for (uint32_t body = 0; body < 3; ++body)
        {
            const auto handle = skeletal.GetBodyHandle(32, body);
            Check(world.GetBodyPose(handle, positions[body], rotations[body]), "Snapshot limit pose");
            Check(world.GetBodyVelocity(handle, velocities[body], angularVelocities[body]), "Snapshot limit velocity");
        }
        ConstraintState before;
        Check(world.GetConstraintState(limitWrist, before), "Snapshot limit wrist state");

        // 각 제한 위반은 구별된 메시지로 실패하며 허용 pose는 실제 break에 성공한다.
        const auto result = skeletal.BreakConstraint(request);
        if (outside)
        {
            Check(result == BreakStatus::UnsupportedPose,
                twist ? "Anchor-aligned +X twist excess rejected" : "Anchor-aligned +Y swing excess rejected");
            Check(world.GetBodyCount() == 3 && world.GetConstraintCount() == 2 &&
                skeletal.GetTopologyGeneration(32) == 1 && skeletal.GetConstraintHandle(32, 1) == limitWrist &&
                skeletal.GetConstraintHandle(32, 0) != InvalidConstraint && limitImpl->PairFilter->Pairs.size() == 2,
                "Limit rejection preserves topology and pairs");
            Check(world.GetConstraintState(limitWrist, state) && state.Enabled == before.Enabled && state.MotorsOff == before.MotorsOff,
                "Limit rejection preserves wrist state");
            for (uint32_t body = 0; body < 3; ++body)
            {
                const auto handle = skeletal.GetBodyHandle(32, body);
                Check(world.GetBodyMotion(handle, motion) && motion == BodyMotion::Kinematic, "Limit rejection preserves motion");
                Check(world.GetBodyPose(handle, p, q) && p == positions[body] && q == rotations[body], "Limit rejection preserves pose");
                Check(world.GetBodyVelocity(handle, v, w) && v == velocities[body] && w == angularVelocities[body],
                    "Limit rejection preserves velocity");
            }
        }
        else
        {
            Check(result == BreakStatus::Applied, twist ? "Inside +X twist accepted" : "Inside +Y swing accepted");
            Check(world.GetConstraintCount() == 1 && skeletal.GetTopologyGeneration(32) == 2, "Inside limit publishes break");
            Check(world.GetConstraintState(limitWrist, state) && state.Enabled && state.MotorsOff, "Inside limit enables wrist");
        }
        Check(skeletal.Unregister(32), "Limit pose fixture released");
        CheckEmpty(world, "Limit pose resources zero");
    }

    // 외부 pin은 거절하고 broken 경계 밖 자손은 후속 break에서 제외한다.
    Check(skeletal.Register(33, fixture.Asset, fixture.NodeParents, frames), "Boundary fixture registered");
    request = { 33, 0, skeletal.GetRigGeneration(33), 1 };
    const auto pin = world.CreateSwingTwist({ skeletal.GetBodyHandle(33, 0),
        skeletal.GetBodyHandle(33, 2), fixture.Asset.Joints[0] });
    Check(pin != InvalidConstraint, "Unexpected pin created");
    Check(skeletal.BreakConstraint(request) == BreakStatus::UnsupportedTopology, "Unexpected external pin rejected");
    Check(world.GetConstraintCount() == 3 && skeletal.GetTopologyGeneration(33) == 1, "Pin rejection leaves topology");
    Check(world.RemoveConstraint(pin), "Unexpected pin removed");
    request = { 33, 1, skeletal.GetRigGeneration(33), 1 };
    Check(skeletal.BreakConstraint(request) == BreakStatus::Applied, "Distal break first");
    const auto detached = skeletal.GetBodyHandle(33, 2);
    Check(world.SetBodyVelocity(detached, { 1, 2, 3 }, {}), "Detached velocity");
    Check(world.GetBodyVelocity(detached, savedV, savedW), "Snapshot detached body");
    request.JointId = 0;
    request.ExpectedTopologyGeneration = 2;
    Check(skeletal.BreakConstraint(request) == BreakStatus::Applied, "Proximal break excludes detached subtree");
    Check(world.GetBodyVelocity(detached, v, w) && v == savedV && w == savedW, "Detached subtree unchanged");
    Check(skeletal.Unregister(33), "Boundary fixture released");
    Check(skeletal.BreakConstraint(request) == BreakStatus::NotFound, "Deleted owner rejected");
    CheckEmpty(world, "Break tests resources zero");
}

// 등록·목표 갱신·본 writeback과 실패 뒤 자원 정리를 검사한다.
int main()
{
    // fixture와 Xbot 왼팔 자산, 각 body의 초기 월드 원점 frame을 준비한다.
    PhysicsWorld world;
    world.Initialize(16);
    SkeletalPhysicsSystem skeletal(world);
    const auto fixture = CreateThreeBodyFixture();
    const auto fixtureFrames = FixtureBodyFrames(fixture);
    std::vector<uint32_t> xbotParents(70, InvalidSkeletalId);
    xbotParents[13] = 12;
    xbotParents[14] = 13;
    xbotParents[15] = 14;
    SkeletalPhysicsAsset xbot;
    Check(CreateXbotLeftArmAsset(xbotParents, xbot), "Xbot asset");
    std::vector<SkeletalRigidFrame> xbotFrames(3);
    xbotFrames[0].Position = { 0.2468535, 5, 3 };
    xbotFrames[1].Position = { 0.493707 + 0.251174, 5, 3 };
    xbotFrames[2].Position = { 0.493707 + 0.502348 + 0.09, 5, 3 };
    const auto* impl = PhysicsWorldAccess::GetImpl(world);

    // 잘못된 owner·frame 수·자산 구조는 자원을 만들지 않고 거절한다.
    Check(!skeletal.Register(Viewer::InvalidObjectId, fixture.Asset, fixture.NodeParents, fixtureFrames), "Invalid owner rejected");
    Check(!skeletal.Register(1, fixture.Asset, fixture.NodeParents, { fixtureFrames[0] }), "Frame count mismatch rejected");
    auto badParents = fixture.NodeParents;
    badParents[1] = InvalidSkeletalId;
    Check(!skeletal.Register(1, fixture.Asset, badParents, fixtureFrames), "Ancestry mismatch rejected");
    auto badFrames = fixtureFrames;
    badFrames[2].Rotation = { 0, 0, 0, 0 };
    Check(!skeletal.Register(1, fixture.Asset, fixture.NodeParents, badFrames), "Bad frame rejected after prepare");
    Check(skeletal.GetRigCount() == 0, "Rejected registration leaves no rig");
    CheckEmpty(world, "Rejected registration leaves no resources");

    // fixture 등록은 body 3·disabled 관절 2·인접 pair 2를 만들고 frame과 질량을 지킨다.
    Check(skeletal.Register(1, fixture.Asset, fixture.NodeParents, fixtureFrames), "Fixture registered");
    Check(world.GetBodyCount() == 3 && world.GetConstraintCount() == 2 && impl->PairFilter->Pairs.size() == 2, "Fixture resources");
    Check(world.GetExcludedPairCount() == 2, "Public excluded pair count");
    Check(skeletal.IsRegistered(1) && skeletal.GetRigGeneration(1) == 1, "Fixture generation");
    Check(!skeletal.Register(1, xbot, xbotParents, xbotFrames) && world.GetBodyCount() == 3, "Duplicate owner rejected");
    for (uint32_t i = 0; i < 2; ++i)
    {
        ConstraintState state{ true, false };
        const auto joint = skeletal.GetConstraintHandle(1, i);
        Check(world.GetConstraintState(joint, state) && !state.Enabled && state.MotorsOff, "Kinematic rig joint disabled");
    }
    for (uint32_t i = 0; i < 3; ++i)
    {
        const auto body = skeletal.GetBodyHandle(1, i);
        std::array<double, 3> position{};
        std::array<double, 4> rotation{};
        Check(world.GetBodyPose(body, position, rotation), "Read fixture pose");
        CheckNear(position[1], fixtureFrames[i].Position[1], 1e-6, "Fixture body frame");
        Check(world.MoveKinematic(body, position, rotation, 1.0f / 60), "Fixture body is kinematic");
    }
    Check(skeletal.GetBodyHandle(1, 3) == InvalidBody && skeletal.GetConstraintHandle(1, 2) == InvalidConstraint, "Out of range id");

    // Xbot rig는 다른 owner로 공존하고 Dynamic 전환 뒤 자산 질량을 가진다.
    Check(skeletal.Register(2, xbot, xbotParents, xbotFrames) && skeletal.GetRigGeneration(2) == 2, "Xbot registered");
    Check(world.GetBodyCount() == 6 && world.GetConstraintCount() == 4 && impl->PairFilter->Pairs.size() == 4, "Two rigs coexist");
    for (uint32_t i = 0; i < 3; ++i)
    {
        double mass = 0;
        const auto body = skeletal.GetBodyHandle(2, i);
        Check(world.SetBodyMotion(body, BodyMotion::Dynamic) && world.GetBodyMass(body, mass), "Xbot dynamic mass");
        CheckNear(mass, xbot.Bodies[i].MassKg, 1e-4 * xbot.Bodies[i].MassKg, "Explicit asset mass");
    }

    // 한 owner 해제는 그 rig의 자원만 정리하고 다른 rig는 보존한다.
    Check(skeletal.Unregister(1) && !skeletal.Unregister(1), "Unregister once");
    Check(world.GetBodyCount() == 3 && world.GetConstraintCount() == 2 && impl->PairFilter->Pairs.size() == 2, "Other rig kept");
    Check(skeletal.Unregister(2) && skeletal.GetRigCount() == 0 && skeletal.GetRigGeneration(2) == 0, "Xbot unregistered");
    CheckEmpty(world, "Unregister frees all resources");

    // 생성 단계마다 실패를 주입하여 이미 만든 자원이 역순으로 모두 정리되는지 확인한다.
    for (uint32_t step = 1; step <= 7; ++step)
    {
        SkeletalPhysicsSystem::DiagFailRegistrationStep = step;
        Check(!skeletal.Register(3, fixture.Asset, fixture.NodeParents, fixtureFrames), "Injected failure rejected");
        Check(!skeletal.IsRegistered(3), "Injected failure leaves no rig");
        CheckEmpty(world, "Injected failure rolls back resources");
    }
    SkeletalPhysicsSystem::DiagFailRegistrationStep = 0;

    // 용량 부족으로 준비 중 실패해도 예약을 남기지 않고 다시 등록하면 새 세대를 받는다.
    PhysicsWorld small;
    small.Initialize(2);
    SkeletalPhysicsSystem tight(small);
    Check(!tight.Register(1, fixture.Asset, fixture.NodeParents, fixtureFrames), "Capacity failure rejected");
    CheckEmpty(small, "Capacity failure leaves no reservation");
    Check(skeletal.Register(3, fixture.Asset, fixture.NodeParents, fixtureFrames) && skeletal.GetRigGeneration(3) == 3, "Retry gets new generation");

    // Clear와 소멸자 순서에서도 world 자원이 남지 않는다.
    Check(skeletal.Register(4, xbot, xbotParents, xbotFrames), "Register before clear");
    skeletal.Clear();
    Check(skeletal.GetRigCount() == 0, "Clear removes rigs");
    CheckEmpty(world, "Clear frees all resources");
    {
        SkeletalPhysicsSystem scoped(world);
        Check(scoped.Register(5, fixture.Asset, fixture.NodeParents, fixtureFrames), "Scoped register");
    }
    CheckEmpty(world, "Destructor frees all resources");
    CheckPoseTargets(world, skeletal);

    // 물리 본 override를 실제 부모 우선 pose 평가기에 전달한다.
    CheckPoseWriteback(world, skeletal);

    // 동기 분리의 성공과 실패 경로를 검사한다.
    CheckSynchronousBreak(world, skeletal);

    // 뷰어 진단에 전달할 실제 물리 상태와 실패 보존을 검사한다.
    CheckDiagnosticState(world, skeletal);
    std::printf("SkeletalBreakTests: %zu/%zu PASS\n", g_Checks, g_Checks);
    return 0;
}
