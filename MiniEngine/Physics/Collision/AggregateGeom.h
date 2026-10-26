#pragma once
#include "ConvexElem.h"
#include <vector>

namespace MiniCollision
{
    // Unreal FKAggregateGeom 중 볼록 요소 목록만 옮긴 충돌 형상 묶음이다.
    struct AggregateGeom
    {
        std::vector<ConvexElem> ConvexElems;
    };

    // 반지름 R(m)에 대한 초기 snap 거리제곱(m²)을 돌려준다. Unreal 0.0015·R_cm(cm² 비교)를 환산한 값이다.
    double KDopSnapDistSq(double radiusMeters);

    // 점 집합을 감싸는 Unreal 임포트 기본값 18-DOP 볼록 요소 하나를 out에 만든다. 점이 없거나 hull 생성에 실패하면 out을 비우고 false를 돌려준다.
    // 18개 반공간 교집합과의 동일성은 정점 병합 전 다면체에만 성립하며, 최종 hull은 병합(0.001 m)과 snap이 반영된 근사다.
    bool GenerateKDop18(const std::vector<Vec3d>& points, AggregateGeom& out);

    // 상자가 평면의 양(+1, 전부 앞), 음(-1, 전부 뒤), 걸침(0) 중 어디인지 돌려준다. Unreal BoxPlaneCompare와 같이 허용 오차 없이 엄격 비교한다.
    // 중심 PlaneDot을 상자 반크기를 법선에 투영한 길이(BoxPushOut)와 비교하므로 길이 단위 변환이 필요 없다.
    int BoxPlaneCompare(const Box& box, const Plane& plane);

    // 볼록 요소를 평면의 양(PlaneDot >= 0) 쪽으로 잘라 out에 만든다. Unreal SliceConvexElem이 뒤집은 평면을 추가해 InProcMesh 쪽(양의 쪽)을 남기는 것과 같다.
    // 전제: slicePlane.Normal은 단위 길이이고 VertexData와 같은 컴포넌트 로컬 좌표다(호출자가 법선과 W를 함께 정규화). 평면이 4개 미만이거나 결과 정점이 4개 미만이면 false다.
    bool SliceConvexElem(const ConvexElem& in, const Plane& slicePlane, ConvexElem& out);

    // 모든 볼록 요소를 평면 양의 쪽(keep)과 음의 쪽(other)으로 나눈다. 통째로 한쪽이면 요소를 그대로 옮기고, 걸치면 양쪽을 각각 자른다.
    // 실패하거나 빈 결과는 버리므로 어느 쪽이든 비어 있을 수 있다(정책은 호출자 몫). 전제는 SliceConvexElem과 같다.
    void SliceAggregateGeom(const AggregateGeom& in, const Plane& slicePlane, AggregateGeom& keep, AggregateGeom& other);
}
