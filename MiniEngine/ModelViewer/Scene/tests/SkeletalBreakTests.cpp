// VS Developer PowerShell, 저장소 루트: CPU 산출물은 TEMP/BreakConstraintTests에만 둔다.
// $bc = Join-Path $env:TEMP 'BreakConstraintTests'; New-Item -ItemType Directory -Force $bc | Out-Null
// $sdk = 'MiniEngine/Build/ThirdParty/Jolt/build'
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /MDd /DJPH_OBJECT_LAYER_BITS=16 /I MiniEngine/Physics /I "$sdk/_deps/joltsource-src" "/Fo:$bc/" "/Fe:$bc/SkeletalBreakTests.exe" MiniEngine/ModelViewer/Scene/tests/SkeletalBreakTests.cpp MiniEngine/ModelViewer/Scene/SkeletalPhysicsSystem.cpp MiniEngine/Physics/PhysicsWorld.cpp MiniEngine/Physics/PhysicsWorldBodies.cpp MiniEngine/Physics/PhysicsWorldConstraints.cpp MiniEngine/Physics/PhysicsWorldStep.cpp MiniEngine/Physics/Collision/ConvexElem.cpp MiniEngine/Physics/Collision/AggregateGeom.cpp MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.cpp "$sdk/lib/Debug/Jolt.lib" /link "/PDB:$bc/SkeletalBreakTests.pdb"
// & "$bc/SkeletalBreakTests.exe"
// Release는 /MDd 대신 /MD /O2 /DNDEBUG, lib/Release/Jolt.lib를 사용한다.
#include "../../../Physics/tests/CollisionTestUtils.h"
#include "../../../Physics/PhysicsWorldInternal.h"
#include "../SkeletalPhysicsSystem.h"
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

// 등록 성공·거절·부분 실패 역정리·해제 뒤 자원 0을 검사한다.
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
    std::printf("SkeletalBreakTests: %zu/%zu PASS\n", g_Checks, g_Checks);
    return 0;
}
