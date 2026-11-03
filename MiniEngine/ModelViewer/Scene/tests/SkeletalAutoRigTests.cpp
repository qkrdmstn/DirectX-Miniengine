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

// 팔레트·거절·frame·anchor와 pose 변형 후의 정점 bounds 및 fallback을 검사한다.
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

    // 비정규화 혼합 weight와 inverse bind를 적용한 segment 및 leaf bounds를 검증한다.
    pose.Parents = { UINT32_MAX, 0 };
    pose.Influencing = { 1, 1 };
    pose.Nodes = { Matrix4(kIdentity), Matrix4(Matrix3(kIdentity), Vector3(4, 0, 0)) };
    SkinCpuSection vertices = {
        { { 0, -2, -1 }, { 0, 1, 0, 0 }, { 10, 30, 0, 0 } },
        { { 2, 2, 3 }, { 0, 1, 0, 0 }, { 30, 10, 0, 0 } },
        { { 999, 999, 999 }, {}, {} } };
    pose.Sections = { { e, { 0, 1 }, &vertices,
        { Matrix4(kIdentity), Matrix4(Matrix3(kIdentity), Vector3(-2, 0, 0)) } } };
    unsigned fallbacks = 99;
    Check(BuildSkeletalPhysicsAsset(pose, asset, &fallbacks) && fallbacks == 0);
    Check(MiniPhysics::ValidateSkeletalPhysicsAsset(asset, pose.Parents));
    const auto& segment = asset.Bodies[0];
    Check(std::abs(segment.HalfExtentMeters[0] - 1) < 2e-5);
    Check(std::abs(segment.HalfExtentMeters[1] - 4) < 2e-5);
    Check(std::abs(segment.HalfExtentMeters[2] - 6) < 2e-5);
    const auto& leafBody = asset.Bodies[1];
    Check(std::abs(leafBody.HalfExtentMeters[0] - 2.5) < 2e-5);
    Check(std::abs(leafBody.HalfExtentMeters[1] - 4) < 2e-5);
    Check(std::abs(leafBody.HalfExtentMeters[2] - 4) < 2e-5);
    Check(std::abs(leafBody.BoneFromBody.Position[0] + 2.5) < 2e-5);
    Check(std::abs(leafBody.BoneFromBody.Position[2] - 2) < 2e-5);

    // 다른 section의 같은 NodeId도 합치고 정점 없는 본의 fallback 수를 확인한다.
    SkinCpuSection extra = { { { 0, 8, 0 }, {}, { 1, 0, 0, 0 } } };
    pose.Sections.push_back({ e, { 0 }, &extra, { Matrix4(kIdentity) } });
    Check(BuildSkeletalPhysicsAsset(pose, asset, &fallbacks) && fallbacks == 0);
    Check(std::abs(asset.Bodies[0].HalfExtentMeters[1] - 16) < 2e-5);
    pose.Sections.resize(1);
    for (auto& vertex : vertices) vertex.Weights[1] = 0;
    Check(BuildSkeletalPhysicsAsset(pose, asset, &fallbacks) && fallbacks == 1);
    bad = pose;
    bad.Sections[0].InverseBind.clear();
    Check(!BuildSkeletalPhysicsAsset(bad, asset));
    vertices[0].Joints[0] = 2;
    Check(!BuildSkeletalPhysicsAsset(pose, asset));
    std::printf("SP-7a2 PASS checks=%u\n", g_Checks);
}
