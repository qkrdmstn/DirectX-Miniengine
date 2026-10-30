// VS Developer PowerShell, 저장소 루트: CPU 산출물은 TEMP/BreakConstraintTests에만 둔다.
// $bc = Join-Path $env:TEMP 'BreakConstraintTests'; New-Item -ItemType Directory -Force $bc | Out-Null
// $sdk = 'MiniEngine/Build/ThirdParty/Jolt/build'
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /MDd /DJPH_OBJECT_LAYER_BITS=16 /I MiniEngine/Physics /I MiniEngine/Core /I "$sdk/_deps/joltsource-src" "/Fo:$bc/" "/Fe:$bc/SkeletalBreakTests.exe" MiniEngine/ModelViewer/Scene/tests/SkeletalBreakTests.cpp MiniEngine/ModelViewer/Scene/SkeletalPhysicsSystem.cpp MiniEngine/Physics/PhysicsWorld.cpp MiniEngine/Physics/PhysicsWorldBodies.cpp MiniEngine/Physics/PhysicsWorldConstraints.cpp MiniEngine/Physics/PhysicsWorldStep.cpp MiniEngine/Physics/Collision/ConvexElem.cpp MiniEngine/Physics/Collision/AggregateGeom.cpp MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.cpp "$sdk/lib/Debug/Jolt.lib" /link "/PDB:$bc/SkeletalBreakTests.pdb"
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
        impl->PairFilter->Pairs.empty(), message);
}

// BoneFromBody가 identity인 fixture는 body 원점 frame이 해당 본의 rest frame과 같다.
static std::vector<SkeletalRigidFrame> FixtureBodyFrames(const SkeletalPhysicsFixture& fixture)
{
    // body ID 순서로 대응 본의 frame을 모은다.
    std::vector<SkeletalRigidFrame> frames;
    for (const auto& body : fixture.Asset.Bodies) frames.push_back(fixture.RestNodeFrames[body.Node]);
    return frames;
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
    std::printf("SkeletalBreakTests: %zu/%zu PASS\n", g_Checks, g_Checks);
    return 0;
}
