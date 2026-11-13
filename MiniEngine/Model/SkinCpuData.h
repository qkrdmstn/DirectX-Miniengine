#pragma once
#include <cstdint>
#include <vector>

// GPU 원본과 같은 mesh-local 위치와 압축 skin 속성만 보관한다.
struct SkinCpuVertex
{
    float Position[3];
    uint16_t Joints[4];
    uint16_t Weights[4];
};
static_assert(sizeof(SkinCpuVertex) == 28, "minimal CPU skin vertex");
struct SkinCpuSection : std::vector<SkinCpuVertex>
{
    using std::vector<SkinCpuVertex>::vector;
    uint32_t StartJoint = 0; // 모델 전체 본 팔레트에서 이 section의 시작 위치
    uint32_t NumJoints = 0;
    std::vector<uint32_t> Indices; // baseVertex를 반영한 section 로컬 삼각형 인덱스
};
