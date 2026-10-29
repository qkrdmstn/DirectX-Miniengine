// VS Developer PowerShell, repository root (Jolt/Model 없이 실행):
// $bcTestDir = Join-Path $env:TEMP 'BreakConstraintTests'
// New-Item -ItemType Directory -Force -Path $bcTestDir | Out-Null
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /I MiniEngine/Physics "/Fo:$bcTestDir/" "/Fe:$bcTestDir/SkeletalAssetTests.exe" MiniEngine/Physics/tests/SkeletalAssetTests.cpp MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.cpp
// & "$bcTestDir/SkeletalAssetTests.exe"
#include "Skeletal/SkeletalPhysicsAsset.h"
#include <cstdio>
#include <cmath>
#include <limits>
#include <utility>

using namespace MiniPhysics;

// xyzw 회전으로 점을 돌려 테스트용 강체 frame의 월드 위치를 구한다.
static std::array<double, 3> TransformPoint(const SkeletalRigidFrame& frame,
    const std::array<double, 3>& point)
{
    // 단위 쿼터니언의 두 번의 외적 식으로 회전한 뒤 위치를 더한다.
    const auto& q = frame.Rotation;
    const std::array<double, 3> cross{
        q[1] * point[2] - q[2] * point[1],
        q[2] * point[0] - q[0] * point[2],
        q[0] * point[1] - q[1] * point[0] };
    return { frame.Position[0] + point[0] + 2 * (q[3] * cross[0] + q[1] * cross[2] - q[2] * cross[1]),
        frame.Position[1] + point[1] + 2 * (q[3] * cross[1] + q[2] * cross[0] - q[0] * cross[2]),
        frame.Position[2] + point[2] + 2 * (q[3] * cross[2] + q[0] * cross[1] - q[1] * cross[0]) };
}

// 생성 fixture와 Xbot rig 값 및 최소 자산의 거절 경계를 검사한다.
int main()
{
    // body 없는 중간 본과 tip을 가진 시험 전용 자산을 구성한다.
    const std::vector<uint32_t> parents{ InvalidSkeletalId, 0, 1, 2, 3 };
    SkeletalPhysicsAsset valid;
    for (uint32_t node : { 0u, 2u, 3u })
    {
        BodyDefinition body;
        body.Node = node;
        body.HalfExtentMeters = { 0.1, 0.25, 0.1 };
        body.MassKg = 1;
        valid.Bodies.push_back(body);
    }
    for (uint32_t child : { 1u, 2u })
    {
        JointDefinition joint;
        joint.ParentBody = child - 1;
        joint.ChildBody = child;
        joint.SwingHalfAngles = { 1, 1 };
        joint.TwistMin = -1;
        joint.TwistMax = 1;
        valid.Joints.push_back(joint);
    }

    // 정상 자산과 독립 단일 body 및 순서가 다른 ID도 허용한다.
    unsigned checks = 0;
    unsigned failures = 0;
    // 검사 결과를 누적하고 실패한 항목 이름을 출력한다.
    const auto check = [&](bool result, const char* name)
    {
        // 성공 여부와 관계없이 실행한 검사 수를 기록한다.
        ++checks;
        if (!result)
        {
            ++failures;
            std::printf("FAILED: %s\n", name);
        }
    };
    check(ValidateSkeletalPhysicsAsset(valid, parents), "intermediate bone and tip");
    auto single = valid;
    single.Bodies.resize(1);
    single.Joints.clear();
    check(ValidateSkeletalPhysicsAsset(single, parents), "single body");
    auto reordered = valid;
    std::swap(reordered.Bodies[0], reordered.Bodies[2]);
    reordered.Joints[0].ParentBody = 2;
    reordered.Joints[1].ChildBody = 0;
    check(ValidateSkeletalPhysicsAsset(reordered, parents), "unordered body IDs");

    // 실제 fixture의 본 위치와 상자 및 연결 끝점이 계약과 일치하는지 확인한다.
    constexpr double pi = 3.14159265358979323846;
    const auto fixture = CreateThreeBodyFixture();
    check(ValidateSkeletalPhysicsAsset(fixture.Asset, fixture.NodeParents), "fixture tree");
    check(fixture.Asset.Bodies.size() == 3 && fixture.Asset.Joints.size() == 2 &&
        fixture.NodeParents == std::vector<uint32_t>{ InvalidSkeletalId, 0, 1, 2 } &&
        fixture.RestNodeFrames.size() == 4, "fixture counts and body-free tip");
    for (uint32_t id = 0; id < 3; ++id)
    {
        const auto& body = fixture.Asset.Bodies[id];
        check(body.Node == id && body.MassKg == 1 &&
            body.HalfExtentMeters == std::array<double, 3>{ 0.1, 0.25, 0.1 } &&
            body.BoneFromBody.Position == std::array<double, 3>{} &&
            body.BoneFromBody.Rotation == std::array<double, 4>{ 0, 0, 0, 1 }, "fixture body");
        check(fixture.RestNodeFrames[id].Position == std::array<double, 3>{ 0, 2 - 0.5 * id, 0 },
            "fixture rest center");
    }
    check(fixture.RestNodeFrames[3].Position == std::array<double, 3>{ 0, 0.5, 0 }, "fixture tip");
    for (uint32_t id = 0; id < 2; ++id)
    {
        const auto& joint = fixture.Asset.Joints[id];
        const double parentY = fixture.RestNodeFrames[joint.ParentBody].Position[1] +
            joint.ParentBodyFromJoint.Position[1];
        const double childY = fixture.RestNodeFrames[joint.ChildBody].Position[1] +
            joint.ChildBodyFromJoint.Position[1];
        check(parentY == childY && parentY == 1.75 - 0.5 * id, "fixture anchors coincide");
        check(joint.ParentBody == id && joint.ChildBody == id + 1 &&
            joint.SwingHalfAngles == std::array<double, 2>{ pi / 3, pi / 3 } &&
            joint.TwistMin == -pi / 4 && joint.TwistMax == pi / 4, "fixture joint limits");
    }

    // Xbot 고정 구조와 월드 m 치수 및 native +X 축 관절을 검사한다.
    std::vector<uint32_t> xbotParents(70, InvalidSkeletalId);
    xbotParents[13] = 12;
    xbotParents[14] = 13;
    xbotParents[15] = 14;
    SkeletalPhysicsAsset xbot;
    check(CreateXbotLeftArmAsset(xbotParents, xbot) &&
        ValidateSkeletalPhysicsAsset(xbot, xbotParents), "Xbot asset");
    check(xbot.Bodies.size() == 3 && xbot.Joints.size() == 2, "Xbot counts");
    const double lengths[] = { 0.493707, 0.502348, 0.18 };
    const double masses[] = { 2, 1.5, 0.5 };
    const double widths[] = { 0.07, 0.06, 0.07 };
    const double depths[] = { 0.07, 0.06, 0.035 };
    for (uint32_t id = 0; id < 3; ++id)
    {
        const auto& body = xbot.Bodies[id];
        check(body.Node == 13 + id && body.MassKg == masses[id] &&
            body.HalfExtentMeters == std::array<double, 3>{ lengths[id] / 2, widths[id], depths[id] },
            "Xbot explicit box and mass");
        check(body.BoneFromBody.Position == std::array<double, 3>{ lengths[id] / 2, 0, 0 } &&
            body.BoneFromBody.Rotation == std::array<double, 4>{ 0, 0, 0, 1 }, "Xbot body offset");
    }
    for (uint32_t id = 0; id < 2; ++id)
    {
        const auto& joint = xbot.Joints[id];
        check(joint.ParentBody == id && joint.ChildBody == id + 1 &&
            joint.ParentBodyFromJoint.Position == std::array<double, 3>{ lengths[id] / 2, 0, 0 } &&
            joint.ChildBodyFromJoint.Position == std::array<double, 3>{ -lengths[id + 1] / 2, 0, 0 },
            "Xbot anchors at next bone origin");
        check(joint.ParentBodyFromJoint.Rotation == std::array<double, 4>{ 0, 0, 0, 1 } &&
            joint.ChildBodyFromJoint.Rotation == std::array<double, 4>{ 0, 0, 0, 1 }, "Xbot native axes");
    }
    const auto& wrist = xbot.Joints[1];
    check(wrist.SwingHalfAngles[0] == pi / 3 && wrist.SwingHalfAngles[1] == pi / 3 &&
        wrist.SwingHalfAngles[0] > 0.126 && wrist.SwingHalfAngles[1] > 0.126 &&
        wrist.TwistMin == -pi / 2 && wrist.TwistMax == pi / 2 &&
        wrist.TwistMin < -0.594 && wrist.TwistMax > -0.574, "BC-00 wrist range included");
    check(xbot.Joints[0].SwingHalfAngles == std::array<double, 2>{ 5 * pi / 6, 5 * pi / 6 } &&
        xbot.Joints[0].TwistMin == -pi / 2 && xbot.Joints[0].TwistMax == pi / 2, "elbow broad limits");

    // GLB 정적 자식 위치와 BC-00 배율로 반올림 anchor 오차를 확인한다.
    const std::array<double, 3> translations[] = {
        { 27.841522216796875, -7.6970929512754083e-5, 3.7516962038353086e-5 },
        { 28.328838348388672, -6.5083577283076011e-7, 3.6357312637846917e-5 } };
    for (uint32_t id = 0; id < 2; ++id)
    {
        // 자식 본이 회전해도 body 중심 offset과 음수 anchor가 서로 상쇄된다.
        SkeletalRigidFrame bone;
        bone.Rotation = { 0, 0, std::sin(0.3), std::cos(0.3) };
        for (uint32_t axis = 0; axis < 3; ++axis)
            bone.Position[axis] = translations[id][axis] * 0.0177327581;
        auto body = bone;
        body.Position = TransformPoint(bone, xbot.Bodies[id + 1].BoneFromBody.Position);
        const auto anchor = TransformPoint(body, xbot.Joints[id].ChildBodyFromJoint.Position);
        double errorSquared = 0;
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const double error = anchor[axis] - (axis == 0 ? lengths[id] : 0);
            errorSquared += error * error;
        }
        check(std::sqrt(errorSquared) <= 1e-4, "rounded Xbot anchors within 0.1mm");
    }

    // 잘못된 Xbot 구조를 거절하고 기존 출력의 구별 가능한 값을 보존한다.
    auto sentinel = valid;
    sentinel.Bodies[0].MassKg = 123;
    for (uint32_t node : { 13u, 14u, 15u, 0u, 69u })
    {
        auto badParents = xbotParents;
        badParents[node] = node;
        check(!CreateXbotLeftArmAsset(badParents, sentinel), "Xbot parent mismatch or cycle");
        check(sentinel.Bodies[0].MassKg == 123 && sentinel.Bodies[1].Node == 2 &&
            sentinel.Joints.size() == 2, "failed factory preserves output");
    }
    xbotParents.resize(69);
    check(!CreateXbotLeftArmAsset(xbotParents, sentinel), "Xbot wrong node count");

    // 각 변경은 정상 원본의 복사본에만 적용하여 독립적으로 거절을 검사한다.
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
#define REJECT(edit) \
    { \
        auto a = valid; \
        auto p = parents; \
        edit; \
        check(!ValidateSkeletalPhysicsAsset(a, p), #edit); \
    }
    REJECT(a.Bodies.clear());
    REJECT(p.clear());
    REJECT(p[4] = 99);
    REJECT(p[4] = 4);
    REJECT(p[0] = 4);
    REJECT(a.Bodies[1].Node = 0);
    REJECT(a.Bodies[1].Node = 99);
    REJECT(a.Bodies[1].Node = InvalidSkeletalId);
    REJECT(a.Bodies[0].HalfExtentMeters[0] = 0);
    REJECT(a.Bodies[0].HalfExtentMeters[1] = -1);
    REJECT(a.Bodies[0].HalfExtentMeters[2] = nan);
    REJECT(a.Bodies[0].HalfExtentMeters[0] = inf);
    REJECT(a.Bodies[0].MassKg = 0);
    REJECT(a.Bodies[0].MassKg = -1);
    REJECT(a.Bodies[0].MassKg = nan);
    REJECT(a.Bodies[0].MassKg = inf);
    REJECT(a.Bodies[0].BoneFromBody.Position[0] = nan);
    REJECT(a.Bodies[0].BoneFromBody.Rotation[3] = 2);
    REJECT(a.Joints[0].ParentBody = InvalidSkeletalId);
    REJECT(a.Joints[0].ChildBody = 99);
    REJECT(a.Joints[0].ChildBody = 0);
    REJECT(a.Joints[1].ChildBody = 1);
    REJECT(a.Joints[0].ParentBody = 2);
    REJECT(a.Joints[1].ParentBody = 0);
    REJECT(p[3] = 0);
    REJECT(p[3] = InvalidSkeletalId);
    REJECT(a.Joints.pop_back());
    REJECT(a.Joints.push_back(a.Joints[0]));
    REJECT(a.Joints[0].ParentBodyFromJoint.Position[1] = inf);
    REJECT(a.Joints[0].ChildBodyFromJoint.Rotation[0] = nan);
    REJECT(a.Joints[0].ChildBodyFromJoint.Rotation = {});
    REJECT(a.Joints[0].SwingHalfAngles[0] = -0.1);
    REJECT(a.Joints[0].SwingHalfAngles[1] = 3.2);
    REJECT(a.Joints[0].SwingHalfAngles[0] = nan);
    REJECT(a.Joints[0].TwistMin = -3.2);
    REJECT(a.Joints[0].TwistMax = 3.2);
    REJECT(a.Joints[0].TwistMin = 2);
    REJECT(a.Joints[0].TwistMax = inf);
    REJECT(
        a.Joints[0].SwingHalfAngles = {};
        a.Joints[0].TwistMin = a.Joints[0].TwistMax);
#undef REJECT

    // Release에서도 실패를 종료 코드로 반환하고 검사 수를 남긴다.
    std::printf("SkeletalAssetTests: %u/%u PASS\n", checks - failures, checks);
    return failures == 0 ? 0 : 1;
}
