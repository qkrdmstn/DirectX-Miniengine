// VS Developer PowerShell, 저장소 루트: 산출물은 TEMP/BreakConstraintTests에 둔다.
// $bc = Join-Path $env:TEMP 'BreakConstraintTests'; New-Item -ItemType Directory -Force $bc | Out-Null
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /I MiniEngine/Core "/Fo:$bc/SkeletalFrameTests.obj" "/Fe:$bc/SkeletalFrameTests.exe" MiniEngine/ModelViewer/Scene/tests/SkeletalFrameTests.cpp
// & "$bc/SkeletalFrameTests.exe"
// Release는 /O2 /DNDEBUG를 추가한다. Jolt와 GPU는 필요 없다.
#include "../SkeletalPhysicsFrames.h"
#include <cstdio>
#include <limits>
#include <stdexcept>

using namespace Math;
using namespace Viewer::SkeletalFrames;
using MiniPhysics::SkeletalRigidFrame;
static size_t Checks = 0;

// Release에서도 실패를 감추지 않고 검사 횟수를 센다.
static void Check(bool condition, const char* message)
{
    // 실패를 예외로 보고한다.
    ++Checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAILED: %s\n", message);
        throw std::runtime_error(message);
    }
}

// 전체 행렬 성분을 비교해 회전·이동·배율 왕복을 확인한다.
static void CheckMatrix(const Matrix4& actual, const Matrix4& expected)
{
    // 열벡터 저장 순서의 16개 값을 각각 대조한다.
    DirectX::XMFLOAT4X4 a, b;
    DirectX::XMStoreFloat4x4(&a, actual);
    DirectX::XMStoreFloat4x4(&b, expected);
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            Check(std::abs(a.m[i][j] - b.m[i][j]) < 3e-5f, "Matrix round trip");
}

// 잘못된 frame 거절 시 위치·회전·배율 출력을 보존하는지 확인한다.
static void CheckRejected(const Matrix4& matrix)
{
    // 두 출력에 감시값을 넣어 전체 보존을 검사한다.
    SkeletalRigidFrame frame{ { 7, 8, 9 }, { 0, 0, 0, 1 } };
    const auto before = frame;
    float scale = 123;
    Check(!TrySplitUniformFrame(matrix, frame, scale), "Unsupported frame rejected");
    Check(frame.Position == before.Position && frame.Rotation == before.Rotation && scale == 123, "Failure preserves frame and scale");
}

// uniform scale 분리·월드 m offset·일반 E 왕복과 무효 입력을 검사한다.
int main()
{
    // Xbot 크기의 작은 배율과 회전한 offset을 임의의 E를 통해 왕복한다.
    const Matrix4 e = Matrix4(OrthogonalTransform(Quaternion(Vector3(kYUnitVector), 0.37f), Vector3(3, -2, 5))) * Matrix4::MakeScale(1.7f);
    const Matrix4 worldBone = Matrix4(OrthogonalTransform(Quaternion(Vector3(kZUnitVector), 0.61f), Vector3(4, 2, -3)));
    const SkeletalRigidFrame offset{ { 0.25, -0.07, 0.11 }, { 0, 0, std::sin(0.2), std::cos(0.2) } };
    Matrix4 offsetMatrix;
    Check(TryRigidMatrix(offset, offsetMatrix), "Rotated offset accepted");
    for (float scale : { 1.0f, 0.0177327f, 2.5f })
    {
        const Matrix4 node = Invert(e) * worldBone * Matrix4::MakeScale(scale);
        SkeletalRigidFrame body;
        float storedScale = 0;
        Check(TryBuildBodyFrame(e, node, offset, body, storedScale), "Body frame built");
        Check(std::abs(storedScale - scale) < scale * 1e-5f, "Uniform scale stored");
        Matrix4 bodyMatrix, restored(kIdentity);
        Check(TryRigidMatrix(body, bodyMatrix), "Body rigid matrix");
        CheckMatrix(bodyMatrix, worldBone * offsetMatrix);
        Check(TryRestoreNodePose(e, body, offset, storedScale, restored), "Node restored");
        CheckMatrix(restored, node);
    }

    // 여러 회전과 배율의 E 역행렬에서 부동소수점 반올림을 확인한다.
    for (int i = 1; i <= 30; ++i)
    {
        const Matrix4 variedE = Matrix4(OrthogonalTransform(Quaternion(0.013f * i, 0.021f * i, -0.017f * i), Vector3(2, -3, 4))) * Matrix4::MakeScale(0.1f * i);
        SkeletalRigidFrame body;
        float storedScale = 0;
        const Matrix4 node = Invert(variedE) * worldBone * Matrix4::MakeScale(0.0177327f);
        Check(TryBuildBodyFrame(variedE, node, offset, body, storedScale), "Varied E accepted");
        Matrix4 restored(kIdentity);
        Check(TryRestoreNodePose(variedE, body, offset, storedScale, restored), "Varied E restored");
        CheckMatrix(restored, node);
    }

    // 비균일·전단·반사·퇴화·원근·비유한 행렬은 자동 근사하지 않는다.
    CheckRejected(Matrix4::MakeScale(Vector3(1, 1.001f, 1)));
    CheckRejected(Matrix4::MakeScale(Vector3(-1, 1, 1)));
    CheckRejected(Matrix4::MakeScale(0));
    Matrix4 bad(kIdentity);
    bad.SetY(Vector4(0.01f, 1, 0, 0));
    CheckRejected(bad);
    bad = Matrix4(kIdentity);
    bad.SetX(Vector4(1, 0, 0, 0.01f));
    CheckRejected(bad);
    for (float value : { std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() })
    {
        bad = Matrix4(kIdentity);
        bad.SetW(Vector4(value, 0, 0, 1));
        CheckRejected(bad);
    }

    // float 길이 제곱의 넘침과 언더플로를 피하고 표현 가능한 크고 작은 균일 배율을 보존한다.
    for (float value : { 1e30f, 1e-30f, std::numeric_limits<float>::denorm_min() })
    {
        SkeletalRigidFrame frame;
        float separated = 0;
        Check(TrySplitUniformFrame(Matrix4::MakeScale(value), frame, separated), "Extreme finite uniform accepted");
        Check(separated == value, "Extreme uniform scale preserved");
    }

    // offset과 역변환 입력의 오류도 호출자 출력에 영향을 주지 않는다.
    Matrix4 output(kIdentity);
    auto badOffset = offset;
    badOffset.Rotation = { 0, 0, 0, 0 };
    Check(!TryRigidMatrix(badOffset, output), "Invalid offset rotation rejected");
    CheckMatrix(output, Matrix4(kIdentity));
    Check(!TryRestoreNodePose(Matrix4::MakeScale(0), offset, offset, 1, output), "Singular E rejected");
    Check(!TryRestoreNodePose(e, offset, offset, 0, output), "Zero saved scale rejected");
    CheckMatrix(output, Matrix4(kIdentity));
    SkeletalRigidFrame sentinel{ { 7, 8, 9 }, { 0, 0, 0, 1 } };
    float scale = 123;
    Check(!TryBuildBodyFrame(e, Matrix4(kIdentity), badOffset, sentinel, scale), "Bad body offset rejected");
    Check(sentinel.Position == std::array<double, 3>{ 7, 8, 9 } && scale == 123, "Body failure preserves output");
    std::printf("SkeletalFrameTests: %zu/%zu PASS\n", Checks, Checks);
    return 0;
}
