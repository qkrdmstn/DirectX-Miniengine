// VS Developer 환경: cl /std:c++17 /EHsc /utf-8 /I MiniEngine/Core /Fo:%TEMP%/ /Fe:%TEMP%/SkeletalAutoRigTests.exe MiniEngine/ModelViewer/Scene/tests/SkeletalAutoRigTests.cpp MiniEngine/Physics/Skeletal/SkeletalPhysicsAsset.cpp
#include "../SkeletalAutoRig.h"
#include <stdexcept>
using namespace Math;
using namespace Viewer;
static unsigned g_Checks = 0;

// Release에서도 조건을 검사하고 실패를 명확히 알린다.
static void Check(bool value)
{
    // assert 대신 예외를 사용해 최적화 설정과 무관하게 검사한다.
    ++g_Checks;
    if (!value) throw std::runtime_error("auto rig check failed");
}

// 팔레트 병합과 거절 조건 및 배율을 제거한 frame과 anchor를 검사한다.
int main()
{
    // 회전·이동·균일 배율과 body 없는 중간 본 및 영향 없는 팔레트를 구성한다.
    SkeletalPoseSnapshot pose;
    pose.Parents = { UINT32_MAX, 0, 1, 2, 0 };
    pose.Influencing = { 1, 0, 1, 1, 0 };
    for (unsigned i = 0; i < 5; ++i)
        pose.Nodes.push_back(Matrix4(Matrix3(kIdentity), Vector3(0, float(i), 0)));
    const Matrix4 e = Matrix4(OrthogonalTransform(Quaternion(Vector3(kZUnitVector), 0.7f), Vector3(3, 4, 5))) * Matrix4::MakeScale(2);
    pose.Sections = { { e, { 0, 2, 4 } }, { e, { 2, 3, 0 } } };
    MiniPhysics::SkeletalPhysicsAsset asset;
    Check(BuildSkeletalPhysicsAsset(pose, asset));
    Check(asset.Bodies.size() == 3 && asset.Joints.size() == 2);
    Check(MiniPhysics::ValidateSkeletalPhysicsAsset(asset, pose.Parents));
    std::vector<Matrix4> bodies;
    for (const auto& body : asset.Bodies)
    {
        MiniPhysics::SkeletalRigidFrame frame;
        float scale = 0;
        Matrix4 matrix;
        Check(SkeletalFrames::TryBuildBodyFrame(e, pose.Nodes[body.Node], body.BoneFromBody, frame, scale));
        Check(SkeletalFrames::TryRigidMatrix(frame, matrix) && std::abs(scale - 2) < 1e-5f);
        const unsigned child = body.Node == 0 ? 2 : 3;
        const Vector3 expected = Vector3((e * pose.Nodes[body.Node]).GetW()) +
            (body.Node == 3 ? Vector3(kZero) : Vector3((e * pose.Nodes[child]).GetW()) - Vector3((e * pose.Nodes[body.Node]).GetW())) * 0.5f;
        Check(float(Length(Vector3(matrix.GetW()) - expected)) < 2e-6f);
        bodies.push_back(matrix);
    }

    // 두 관절 anchor는 자식 본 원점과 일치하고 실패 시 이전 자산이 보존된다.
    for (const auto& joint : asset.Joints)
    {
        Matrix4 a, b;
        Check(SkeletalFrames::TryRigidMatrix(joint.ParentBodyFromJoint, a) && SkeletalFrames::TryRigidMatrix(joint.ChildBodyFromJoint, b));
        const Vector3 expected((e * pose.Nodes[asset.Bodies[joint.ChildBody].Node]).GetW());
        Check(float(Length(Vector3((bodies[joint.ParentBody] * a).GetW()) - expected)) < 2e-6f);
        Check(float(Length(Vector3((bodies[joint.ChildBody] * b).GetW()) - expected)) < 2e-6f);
    }
    auto bad = pose;
    bad.Parents[2] = UINT32_MAX;
    Check(!BuildSkeletalPhysicsAsset(bad, asset) && asset.Bodies.size() == 3);
    bad = pose;
    bad.Sections[1].E = Matrix4(kIdentity);
    Check(!BuildSkeletalPhysicsAsset(bad, asset));
    bad = pose;
    bad.Nodes[2] = Matrix4::MakeScale(Vector3(1, 2, 1));
    Check(!BuildSkeletalPhysicsAsset(bad, asset));

    // 분기에서는 가장 먼 영향 자식을 고르고 말단은 자기 원점과 들어오는 길이를 쓴다.
    pose.Parents = { UINT32_MAX, 0, 0 };
    pose.Influencing = { 1, 1, 1 };
    pose.Nodes = { Matrix4(kIdentity), Matrix4(Matrix3(kIdentity), Vector3(0, 2, 0)),
        Matrix4(Matrix3(kIdentity), Vector3(4, 0, 0)) };
    pose.Sections = { { Matrix4(kIdentity), { 2, 0, 1, 2 } } };
    Check(BuildSkeletalPhysicsAsset(pose, asset));
    Check(std::abs(asset.Bodies[0].BoneFromBody.Position[0] - 2) < 1e-6);
    Check(std::abs(asset.Bodies[0].HalfExtentMeters[1] - 0.6) < 1e-6);
    Check(std::abs(asset.Bodies[1].HalfExtentMeters[0] - 1) < 1e-6);
    Check(std::abs(asset.Bodies[2].HalfExtentMeters[0] - 2) < 1e-6);
    for (const auto& body : asset.Bodies)
        if (body.Node) Check(body.BoneFromBody.Position == std::array<double, 3>{});
    std::printf("SP-7a PASS checks=%u\n", g_Checks);
}
