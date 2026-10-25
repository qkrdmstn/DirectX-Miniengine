#pragma once
#include "VectorMath.h"
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
}
