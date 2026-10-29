// VS Developer PowerShell, 저장소 루트: CPU 산출물은 TEMP/BreakConstraintTests에만 둔다.
// $bc = Join-Path $env:TEMP 'BreakConstraintTests'; New-Item -ItemType Directory -Force $bc | Out-Null
// $sdk = 'MiniEngine/Build/ThirdParty/Jolt/build'
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /MDd /DJPH_OBJECT_LAYER_BITS=16 /I MiniEngine/Physics /I "$sdk/_deps/joltsource-src" "/Fo:$bc/" "/Fe:$bc/PhysicsConstraintTests.exe" MiniEngine/Physics/tests/PhysicsConstraintTests.cpp MiniEngine/Physics/PhysicsWorld.cpp MiniEngine/Physics/PhysicsWorldBodies.cpp MiniEngine/Physics/PhysicsWorldConstraints.cpp MiniEngine/Physics/PhysicsWorldStep.cpp MiniEngine/Physics/Collision/ConvexElem.cpp MiniEngine/Physics/Collision/AggregateGeom.cpp MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.cpp "$sdk/lib/Debug/Jolt.lib" /link "/PDB:$bc/PhysicsConstraintTests.pdb"
// & "$bc/PhysicsConstraintTests.exe"
// Release는 /MDd 대신 /MD /O2 /DNDEBUG, lib/Release/Jolt.lib를 사용한다.
#include "CollisionTestUtils.h"
#include "Collision/AggregateGeom.h"
#include "PhysicsWorldInternal.h"
#include <cstdio>
#include <limits>

using namespace MiniPhysics;

// 잠든 Dynamic endpoint가 Parent와 Child 어느 쪽이든 관절 disable 후 낙하하는지 검사한다.
static void CheckSleepingEndpointFalls(bool dynamicParent)
{
    // 중심이 원점인 상자와 중력이 작용하는 독립 world를 준비한다.
    PhysicsWorld world;
    world.Initialize(8);
    AggregateGeom geometry;
    std::vector<Vec3d> corners;
    for (int i = 0; i < 8; ++i)
        corners.push_back({ (i & 1) ? 0.1 : -0.1, (i & 2) ? 0.1 : -0.1, (i & 4) ? 0.1 : -0.1 });
    Check(GenerateKDop18(corners, geometry), "Sleep fixture box cooked");

    // 같은 위치의 Kinematic과 Dynamic을 중심 관절로 연결하고 서로의 충돌을 제외한다.
    BodyHandle handles[2]{};
    const int dynamicIndex = dynamicParent ? 0 : 1;
    for (int i = 0; i < 2; ++i)
    {
        BodyDesc body;
        body.Geometry = &geometry;
        body.Position = { 0, 5, 0 };
        body.Motion = i == dynamicIndex ? BodyMotion::Dynamic : BodyMotion::Kinematic;
        PreparedBody prepared;
        Check(world.PrepareBody(body, prepared), "Prepare sleep endpoint");
        handles[i] = world.CommitBody(std::move(prepared), true);
        Check(handles[i] != InvalidBody, "Commit sleep endpoint");
    }
    SwingTwistDesc desc{ handles[0], handles[1], CreateThreeBodyFixture().Asset.Joints[0] };
    desc.Joint.ParentBodyFromJoint.Position = { 0, 0, 0 };
    desc.Joint.ChildBodyFromJoint.Position = { 0, 0, 0 };
    const auto joint = world.CreateSwingTwist(desc);
    Check(joint != InvalidConstraint && world.ExcludeBodyPair(handles[0], handles[1]), "Suspend dynamic endpoint");

    // 강제 재우기 없이 최대 10초 동안 step하여 Dynamic의 자연 수면을 확인한다.
    const auto dynamic = handles[dynamicIndex];
    for (int tick = 0; tick < 600 && world.IsBodyAwake(dynamic); ++tick)
        Check(world.Step(1.0f / 60), "Wait for suspended endpoint to sleep");
    Check(!world.IsBodyAwake(dynamic), "Suspended dynamic endpoint sleeps");
    std::array<double, 3> before{};
    Check(world.GetBodyCenterOfMass(dynamic, before), "Read sleeping endpoint position");

    // disable 직후 깨어나고 다음 step에서 중력에 의해 위치가 내려가는지 확인한다.
    Check(world.SetConstraintEnabled(joint, false), "Disable sleeping endpoint joint");
    Check(world.IsBodyAwake(dynamic), "Disable wakes dynamic endpoint");
    Check(world.Step(1.0f / 60), "Disabled endpoint gravity step");
    std::array<double, 3> after{};
    Check(world.GetBodyCenterOfMass(dynamic, after), "Read falling endpoint position");
    Check(after[1] < before[1] - 1e-4, "Disabled sleeping endpoint falls");
}

// COM offset을 가진 상자 세 개와 관절 두 개의 수명·frame·필터를 검사한다.
int main()
{
    // 양쪽 endpoint의 수면 중 disable 회귀를 각각 검사한다.
    CheckSleepingEndpointFalls(true);
    CheckSleepingEndpointFalls(false);

    // 입력 형상과 world를 만들고 초기화 전 거절을 검사한다.
    PhysicsWorld world;
    SwingTwistDesc desc;
    ConstraintState state{ true, false };
    Check(world.CreateSwingTwist(desc) == InvalidConstraint, "Uninitialized create rejected");
    Check(!world.GetConstraintState(1, state) && state.Enabled && !state.MotorsOff, "Failure preserves output");
    world.Initialize(32);
    AggregateGeom geometry;
    std::vector<Vec3d> corners;
    for (int i = 0; i < 8; ++i)
        corners.push_back({ 0.3 + ((i & 1) ? 0.1 : -0.1), (i & 2) ? 0.25 : -0.25, (i & 4) ? 0.1 : -0.1 });
    Check(GenerateKDop18(corners, geometry), "Offset box cooked");
    const auto fixture = CreateThreeBodyFixture();
    BodyHandle handles[3]{};
    ConstraintHandle joints[2]{};
    auto* impl = PhysicsWorldAccess::GetImpl(world);

    // Kinematic chain을 만들고 관절의 local COM frame과 축을 검사한다.
    for (int i = 0; i < 3; ++i)
    {
        BodyDesc body;
        body.Geometry = &geometry;
        body.Motion = BodyMotion::Kinematic;
        body.Position = fixture.RestNodeFrames[i].Position;
        PreparedBody prepared;
        Check(world.PrepareBody(body, prepared), "Prepare kinematic");
        handles[i] = world.CommitBody(std::move(prepared), false);
        Check(handles[i] != InvalidBody, "Commit kinematic");
    }
    for (int i = 0; i < 2; ++i)
    {
        desc = { handles[i], handles[i + 1], fixture.Asset.Joints[i] };
        joints[i] = world.CreateSwingTwist(desc);
        Check(joints[i] != InvalidConstraint, "Create swing twist");
        Check(world.GetConstraintState(joints[i], state) && !state.Enabled && state.MotorsOff, "Kinematic joint disabled and motors off");
        Check(!world.SetConstraintEnabled(joints[i], true), "Both kinematic enable rejected");
        const auto& joint = impl->Constraints.at(joints[i]).Joint;
        CheckNear(joint->GetLocalSpacePosition1().GetX(), -0.3, 1e-5, "Parent origin to COM corrected");
        CheckNear(joint->GetLocalSpacePosition2().GetY(), 0.25, 1e-5, "Child anchor preserved");
        CheckNear(joint->GetNormalHalfConeAngle(), fixture.Asset.Joints[i].SwingHalfAngles[0], 1e-6, "Swing limit mapped");
        Check(world.ExcludeBodyPair(handles[i], handles[i + 1]), "Exclude adjacent pair");
    }
    Check(world.GetConstraintCount() == 2 && world.GetBodyCount() == 3, "Three bodies two joints");
    Check(world.ExcludeBodyPair(handles[1], handles[0]) && impl->PairFilter->Pairs.size() == 2, "Pair exclusion symmetric idempotent");
    Check(!world.ExcludeBodyPair(handles[0], handles[0]) && !world.ExcludeBodyPair(0, handles[0]), "Invalid pair rejected");

    // 잘못된 입력을 거절하고 기존 관절 수를 보존한다.
    desc.Parent = desc.Child;
    Check(!world.CreateSwingTwist(desc), "Self joint rejected");
    desc.Parent = 0;
    Check(!world.CreateSwingTwist(desc), "Invalid endpoint rejected");
    desc.Parent = handles[1];
    desc.Joint.TwistMin = std::numeric_limits<double>::quiet_NaN();
    Check(!world.CreateSwingTwist(desc), "NaN limit rejected");
    desc.Joint = fixture.Asset.Joints[1];
    desc.Joint.ChildBodyFromJoint.Rotation = { 0, 0, 0, 0 };
    Check(!world.CreateSwingTwist(desc) && world.GetConstraintCount() == 2, "Bad frame leaves count intact");
    desc.Joint = fixture.Asset.Joints[1];
    desc.Joint.SwingHalfAngles[0] = 4;
    Check(!world.CreateSwingTwist(desc), "Swing out of range rejected");
    desc.Joint.SwingHalfAngles = { 0, 0 };
    desc.Joint.TwistMin = desc.Joint.TwistMax = 0;
    Check(!world.CreateSwingTwist(desc), "Weld rejected");

    // Dynamic 전환 후 모터를 켠 내부 시험 상태를 공통 API로 다시 끈다.
    Check(world.SetBodyMotion(handles[1], BodyMotion::Dynamic), "Dynamic transition");
    Check(world.SetConstraintEnabled(joints[0], true), "Mixed motion enable");
    impl->Constraints.at(joints[0]).Joint->SetSwingMotorState(JPH::EMotorState::Position);
    impl->Constraints.at(joints[0]).Joint->SetTwistMotorState(JPH::EMotorState::Velocity);
    Check(world.SetConstraintMotorsOff(joints[0]), "Motors off API");
    Check(world.GetConstraintState(joints[0], state) && state.Enabled && state.MotorsOff, "Enabled with motors off");
    Check(world.SetBodyMotion(handles[2], BodyMotion::Dynamic) && world.SetConstraintEnabled(joints[1], true), "Enable wrist");
    for (int tick = 0; tick < 120; ++tick)
        Check(world.Step(1.0f / 60), "Constrained chain step");
    const auto& wrist = impl->Constraints.at(joints[1]).Joint;
    auto& interface = impl->World.GetBodyInterface();
    const auto anchor1 = interface.GetCenterOfMassTransform(impl->SimBodies.at(handles[1])) * wrist->GetLocalSpacePosition1();
    const auto anchor2 = interface.GetCenterOfMassTransform(impl->SimBodies.at(handles[2])) * wrist->GetLocalSpacePosition2();
    Check((anchor1 - anchor2).Length() < 0.01f, "Wrist anchor error below 1 cm");
    Check(world.SetBodyMotion(handles[2], BodyMotion::Kinematic), "Restore wrist kinematic");
    Check(world.SetBodyMotion(handles[1], BodyMotion::Kinematic), "Kinematic transition");
    Check(world.GetConstraintState(joints[0], state) && !state.Enabled, "Last dynamic removed disables solver");

    // break처럼 관절만 제거한 뒤 인접 pair가 여전히 접촉하지 않는지 검사한다.
    Check(world.RemoveConstraint(joints[0]) && !world.RemoveConstraint(joints[0]), "Remove and stale rejection");
    Check(!world.SetConstraintEnabled(joints[0], false) && !world.SetConstraintMotorsOff(joints[0]), "Stale mutation rejected");
    Check(world.RemoveConstraint(joints[1]), "Remove wrist for isolated contact test");
    for (auto handle : handles)
    {
        Check(world.SetBodyMotion(handle, BodyMotion::Dynamic), "Contact test dynamic");
        Check(world.SetBodyTransform(handle, { 0, 2, 0 }, { 0, 0, 0, 1 }), "Overlap bodies");
    }
    Check(world.Step(1.0f / 60), "Contact step");
    const auto a = impl->SimBodies.at(handles[0]);
    const auto b = impl->SimBodies.at(handles[1]);
    const auto c = impl->SimBodies.at(handles[2]);
    Check(!impl->World.WereBodiesInContact(a, b) && !impl->World.WereBodiesInContact(b, c), "Adjacent pairs never contact after break");
    Check(impl->World.WereBodiesInContact(a, c), "Nonadjacent pair contacts");

    // 다른 owner에 해당하는 기본 필터 body와 고정 바닥의 접촉은 유지한다.
    BodyDesc otherDesc;
    otherDesc.Geometry = &geometry;
    otherDesc.Position = { 0, 2, 0 };
    PreparedBody otherPrepared;
    Check(world.PrepareBody(otherDesc, otherPrepared), "Prepare other owner");
    const auto other = world.CommitBody(std::move(otherPrepared), true);
    Check(world.SetBodyTransform(handles[0], { 0, 2, 0 }, { 0, 0, 0, 1 }), "Reset adjacent body for other owner");
    Check(world.Step(1.0f / 60), "Other owner contact step");
    Check(impl->World.WereBodiesInContact(a, impl->SimBodies.at(other)), "Other owner still contacts");
    Check(world.DestroyBody(other), "Destroy other owner");
    BodyDesc floorDesc;
    floorDesc.Motion = BodyMotion::Static;
    floorDesc.StaticBoxHalfExtent = { 10, 0.1, 10 };
    floorDesc.Position = { 0, 1.8, 0 };
    PreparedBody floorPrepared;
    Check(world.PrepareBody(floorDesc, floorPrepared), "Prepare floor");
    const auto floor = world.CommitBody(std::move(floorPrepared), false);
    Check(world.SetBodyTransform(handles[0], { 0, 2, 0 }, { 0, 0, 0, 1 }) && world.Step(1.0f / 60), "Floor contact step");
    Check(impl->World.WereBodiesInContact(a, impl->SimBodies.at(floor)), "Floor still contacts");
    desc = { floor, handles[0], fixture.Asset.Joints[0] };
    Check(!world.CreateSwingTwist(desc) && !world.ExcludeBodyPair(floor, handles[0]), "Static endpoint rejected");

    // 이미 있던 접촉도 pair 제외 적용 후 다음 step부터 사라진다.
    Check(world.SetBodyTransform(handles[0], { 0, 5, 0 }, { 0, 0, 0, 1 }), "Reset first cached body");
    Check(world.SetBodyTransform(handles[2], { 0, 5, 0 }, { 0, 0, 0, 1 }) && world.Step(1.0f / 60), "Cached contact step");
    Check(impl->World.WereBodiesInContact(a, c), "Cached pair initially contacts");
    Check(world.ExcludeBodyPair(handles[0], handles[2]) && world.Step(1.0f / 60), "Exclude cached contact");
    Check(!impl->World.WereBodiesInContact(a, c), "Cached pair now excluded");

    // body 제거·Clear·Shutdown에서 관절과 pair가 남지 않고 handle도 재사용되지 않는다.
    desc = { handles[0], handles[1], fixture.Asset.Joints[0] };
    const auto newer = world.CreateSwingTwist(desc);
    const auto& recreated = impl->Constraints.at(newer).Joint;
    CheckNear(recreated->GetTwistMinAngle(), fixture.Asset.Joints[0].TwistMin, 1e-6, "Twist limit mapped");
    Check(newer > joints[1] && world.RemoveBodyFromWorld(handles[0]), "Monotonic handle and endpoint removal");
    Check(world.GetConstraintCount() == 0, "Endpoint removal cleans constraints");
    Check(world.AddBodyToWorld(handles[0], true), "Endpoint readd");
    desc.Joint.ParentBodyFromJoint.Rotation = { 0, 0, std::sqrt(0.5), std::sqrt(0.5) };
    const auto oriented = world.CreateSwingTwist(desc);
    const auto twistAxis = impl->Constraints.at(oriented).Joint->GetConstraintToBody1() * JPH::Vec3::sAxisX();
    CheckNear(twistAxis.GetY(), 1, 1e-5, "Joint rotation maps twist axis");
    Check(world.DestroyBody(handles[1]) && world.GetConstraintCount() == 0, "Destroy endpoint removes all constraints first");
    desc.Child = handles[2];
    Check(world.CreateSwingTwist(desc) != InvalidConstraint, "Recreate before clear");
    world.Clear();
    Check(world.GetConstraintCount() == 0 && world.GetBodyCount() == 0 && impl->PairFilter->Pairs.empty(), "Clear frees all resources");
    world.Shutdown();
    world.Initialize(8);
    Check(!world.GetConstraintState(newer, state), "Shutdown stale handle rejected");

    // 살아 있는 관절을 둔 Shutdown도 body보다 먼저 참조를 해제한다.
    for (int i = 0; i < 2; ++i)
    {
        PreparedBody prepared;
        Check(world.PrepareBody(otherDesc, prepared), "Prepare shutdown fixture");
        handles[i] = world.CommitBody(std::move(prepared), true);
    }
    desc = { handles[0], handles[1], fixture.Asset.Joints[0] };
    const auto shutdownJoint = world.CreateSwingTwist(desc);
    Check(shutdownJoint > newer, "Handle not reused across shutdown");
    world.Shutdown();
    Check(world.GetConstraintCount() == 0 && world.GetBodyCount() == 0, "Shutdown frees live resources");
    std::printf("PhysicsConstraintTests: %zu/%zu PASS\n", g_Checks, g_Checks);
    return 0;
}
