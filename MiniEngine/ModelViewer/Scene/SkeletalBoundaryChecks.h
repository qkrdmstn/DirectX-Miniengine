#pragma once
#include "Scene.h"

namespace Viewer
{
    // C 시점에 추출 삼각형과 유지 삼각형이 함께 쓰는 원본 정점과 그 위치이다.
    struct BoundaryVertex
    {
        uint32_t Section, Index;
        Math::Vector3 Local;
    };

    // 조각 정점과 같은 위치에 있던 원본 경계 정점의 짝이다.
    struct BoundaryPair
    {
        uint32_t Piece, Boundary;
        Math::Vector3 Local;
    };

    // 두 조각의 baked pose를 읽어 C 시점 경계 정점과 같은 위치끼리 짝짓는다.
    bool PairBoundary(const MeshComponent* const pieces[2], const std::vector<BoundaryVertex>& boundary,
        std::vector<BoundaryPair>& pairs);

    // 현재 프레임의 원본 pose와 부착 A/B의 외곽 경계 최대 거리를 잰다.
    bool MeasureBoundary(const GameObject& owner, const MeshComponent& source, const MeshComponent* const pieces[2],
        const std::vector<BoundaryVertex>& boundary, const std::vector<BoundaryPair>& pairs,
        float& maxError);
}
