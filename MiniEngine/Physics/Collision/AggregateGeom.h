#pragma once
#include "ConvexElem.h"
#include <vector>

namespace MiniCollision
{
    // Unreal FKAggregateGeom 중 볼록 요소 목록만 옮긴 충돌 형상 묶음이다. 절단은 이후 단계에서 추가한다.
    struct AggregateGeom
    {
        std::vector<ConvexElem> ConvexElems;
    };

    // 반지름 R(m)에 대한 초기 snap 거리제곱(m²)을 돌려준다. Unreal 0.0015·R_cm(cm² 비교)를 환산한 값이다.
    double KDopSnapDistSq(double radiusMeters);

    // 점 집합을 감싸는 Unreal 임포트 기본값 18-DOP 볼록 요소 하나를 out에 만든다. 점이 없거나 hull 생성에 실패하면 out을 비우고 false를 돌려준다.
    // 18개 반공간 교집합과의 동일성은 정점 병합 전 다면체에만 성립하며, 최종 hull은 병합(0.001 m)과 snap이 반영된 근사다.
    bool GenerateKDop18(const std::vector<Vec3d>& points, AggregateGeom& out);
}
