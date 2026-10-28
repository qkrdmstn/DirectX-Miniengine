#pragma once
#include "VectorMath.h"
#include <array>
#include <cstdint>

namespace MeshCutting
{
    // 원본과 생성 메시가 공유하는 draw 단위 입력 및 고정 출력 구간이다.
    struct CutSection
    {
        uint32_t VbOffset, Stride, IbOffset, Index32;
        uint32_t StartIndex, BaseVertex, Triangles, PoseOffset;
        uint32_t FirstTriangle, VertexStart, VertexCapacity, Flags;
        uint32_t SourceSection, SourceDraw, VertexCount, Reserved;
    };
    static_assert(sizeof(CutSection) == 64, "HLSL section layout");

    // 평면 부호 거리는 월드 단위이며 ProjectU/V는 월드 직교 단면 좌표를 만든다.
    struct CutConstants
    {
        Math::Vector4 Plane;
        Math::Matrix4 LocalToRectangle;
        Math::Vector4 ProjectU, ProjectV;
        float Epsilon, HalfWidth, HalfHeight;
        uint32_t Triangles;
        uint32_t Sections, VertexCapacity, DepthOffset, IndexOffset;
        uint32_t MaxSegments, HashMask, HashOffset, HashLinksOffset;
    };
    static_assert(sizeof(CutConstants) == 160, "HLSL constant layout");

    // Metadata 끝에 붙는 면별 18-DOP 단어 수다(최소 9 + 최대 9 + 기여 정점 수 1).
    constexpr uint32_t KDopWordsPerSide = 19;

    // 면별 18-DOP의 9개 축이다. MiniCollision KDopDir18의 짝수 번째 방향과 같은 순서이며 GPU의 DopAxes와 일치해야 한다.
    constexpr double KDopAxes9[9][3] =
    {
        { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 },
        { 0, 0.70710678118654752440, 0.70710678118654752440 }, { 0, 0.70710678118654752440, -0.70710678118654752440 },
        { 0.70710678118654752440, 0, 0.70710678118654752440 }, { 0.70710678118654752440, 0, -0.70710678118654752440 },
        { 0.70710678118654752440, 0.70710678118654752440, 0 }, { 0.70710678118654752440, -0.70710678118654752440, 0 }
    };

    // 한 면 출력 정점의 축별 투영 최솟값/최댓값이며 음의 방향 9개는 Min이 대신한다.
    struct KDop18
    {
        std::array<float, 9> Min;
        std::array<float, 9> Max;
        uint32_t VertexCount;
    };
}
