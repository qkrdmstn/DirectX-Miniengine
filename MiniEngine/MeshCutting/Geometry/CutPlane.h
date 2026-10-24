#pragma once

#include "VectorMath.h"

namespace MeshCutting
{
    struct CutPlaneDesc
    {
        float Width = 1.0f;
        float Height = 1.0f;
    };

    struct CutPlaneData
    {
        Math::Vector4 WorldPlane; // 정규화된 (normal.xyz, d), normal은 로컬 +Y 방향.
        Math::Matrix4 WorldToPlane;
        float HalfWidth;
        float HalfHeight;
        Math::Vector3 WorldCorners[4]; // 로컬 XZ의 (-,-), (+,-), (+,+), (-,+).
    };

    // 유한 양수인 로컬 XZ 직사각형 크기를 검사한다.
    bool IsValidCutPlaneDesc(const CutPlaneDesc& desc);
    // 가역 affine 변환으로 평면 스냅샷을 만들며 실패 시 출력을 보존한다.
    bool TryBuildCutPlaneData(const CutPlaneDesc& desc, const Math::AffineTransform& transform, CutPlaneData& data);
    // 정규화된 월드 ray의 앞쪽 직사각형 교점을 찾으며 t는 월드 거리를 유지한다.
    bool IntersectCutPlaneRectangle(const CutPlaneData& data, Math::Vector3 worldRayOrigin,
        Math::Vector3 normalizedWorldRayDirection, float& outT);
    // 공면 선분의 직사각형 내부 구간을 원본 선분의 [0,1] 매개변수로 반환한다.
    bool ClipCoplanarSegmentToCutPlaneRectangle(const CutPlaneData& data, Math::Vector3 start,
        Math::Vector3 end, float& outT0, float& outT1);
}
