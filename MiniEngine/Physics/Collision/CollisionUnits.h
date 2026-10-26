#pragma once

// Unreal은 cm, 이 프로젝트는 m이므로 Unreal 상수를 환산해 둔다.
namespace MiniCollision
{
    // 정점 병합 거리. Unreal MIN_HULL_VERT_DISTANCE 0.1 cm (KAggregateGeom.cpp AddVertexIfNotPresent).
    constexpr double MinHullVertDistance = 0.001;

    // 최소 총 면적. Unreal HullFromPlanes의 TotalPolyArea < 0.001 (cm²).
    constexpr double MinHullArea = 1e-7;

    // 기본 snap 거리 제곱. Unreal의 InSnapDistance(UE_SMALL_NUMBER 1e-8)를 제곱거리 cm²로 비교한 값.
    constexpr double DefaultSnapDistSq = 1e-12;

    // 공선 판정(내적 < 1 - eps). Unreal LOCAL_EPS(UE_SMALL_NUMBER 1e-8)의 각도 용도, 무차원이라 환산 없음.
    constexpr double LocalEpsAngle = 1e-8;

    // 공면 판정(|PlaneDot| > eps). Unreal LOCAL_EPS 1e-8 cm의 거리 용도를 m로 환산한 값.
    constexpr double LocalEpsDistance = 1e-10;

    // 평면별 초기 큰 사각형의 반변. Unreal UE_OLD_HALF_WORLD_MAX 1048576 cm (EngineDefines.h).
    constexpr double BigPolygonHalfExtent = 10485.76;

    // 18-DOP 평면 상수 팽창량. Unreal GenerateKDopAsSimpleCollision의 MinSize 0.1 cm를 m로 환산한 값이다.
    constexpr double KDopMinSize = 0.001;

    // 18-DOP 초기 snap 거리제곱의 반지름당 계수. Unreal SuggestConvexVertexSnapDistance의 0.0015·R_cm를 cm² 비교에서 m²로 환산한 값이다.
    constexpr double KDopSnapDistSqPerMeter = 0.000015;
}
