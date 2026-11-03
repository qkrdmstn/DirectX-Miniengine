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
using SkinCpuSection = std::vector<SkinCpuVertex>;
