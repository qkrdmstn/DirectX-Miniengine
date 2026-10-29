// VS Developer PowerShell, repository root (Jolt/Model 없이 실행):
// $bcTestDir = Join-Path $env:TEMP 'BreakConstraintTests'
// New-Item -ItemType Directory -Force -Path $bcTestDir | Out-Null
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /I MiniEngine/Physics "/Fo:$bcTestDir/" "/Fe:$bcTestDir/SkeletalAssetTests.exe" MiniEngine/Physics/tests/SkeletalAssetTests.cpp MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.cpp
// & "$bcTestDir/SkeletalAssetTests.exe"
#include "Skeletal/SkeletalPhysicsAsset.h"
#include <cstdio>
#include <limits>
#include <utility>

using namespace MiniPhysics;

// 유효한 최소 자산에서 항목 하나씩 바꿔 거절 경계를 검사한다.
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
