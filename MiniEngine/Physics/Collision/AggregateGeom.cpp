#include "AggregateGeom.h"
#include <algorithm>

namespace MiniCollision
{
    namespace
    {
        // 18-DOP 방향의 대각 성분. Unreal GeomFitUtils.h의 RCP_SQRT2(1/sqrt 2)와 같은 값이다.
        constexpr double Rcp2 = 0.70710678118654752440;

        // Unreal GeomFitUtils.h KDopDir18과 같은 순서의 단위 방향 18개(축 6개, 대각 12개)다.
        const Vec3d KDopDirs18[18] =
        {
            { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
            { 0, Rcp2, Rcp2 }, { 0, -Rcp2, -Rcp2 }, { 0, Rcp2, -Rcp2 }, { 0, -Rcp2, Rcp2 },
            { Rcp2, 0, Rcp2 }, { -Rcp2, 0, -Rcp2 }, { Rcp2, 0, -Rcp2 }, { -Rcp2, 0, Rcp2 },
            { Rcp2, Rcp2, 0 }, { -Rcp2, -Rcp2, 0 }, { Rcp2, -Rcp2, 0 }, { -Rcp2, Rcp2, 0 }
        };
    }

    // snap 거리제곱을 반지름에 비례해 돌려준다.
    double KDopSnapDistSq(double radiusMeters)
    {
        // 단위 반지름당 계수에 반지름을 곱한다.
        return KDopSnapDistSqPerMeter * radiusMeters;
    }

    // 점 집합의 방향별 최댓값에 KDopMinSize를 더한 평면 18개로 hull을 만든다.
    bool GenerateKDop18(const std::vector<Vec3d>& points, AggregateGeom& out)
    {
        // 이전 결과를 비우고 점이 없으면 실패한다.
        out.ConvexElems.clear();
        if (points.empty())
        {
            return false;
        }

        // 방향마다 모든 점의 최대 투영값에 팽창량을 더해 바깥 법선 평면을 만든다.
        std::vector<Plane> planes;
        for (const Vec3d& dir : KDopDirs18)
        {
            double maxDist = Dot(dir, points[0]);
            for (const Vec3d& p : points)
            {
                maxDist = std::max(maxDist, Dot(dir, p));
            }
            planes.push_back({ dir, maxDist + KDopMinSize });
        }

        // snap 후보로 병합 전 다면체 정점을 모은다(Unreal은 BSP 모델 정점).
        std::vector<Vec3d> candidates;
        CollectPlanePolygonVertices(planes, HullTolerances(), candidates);
        if (candidates.empty())
        {
            return false;
        }

        // 반지름은 후보의 AABB 중심에서 가장 먼 정점까지의 거리다(Unreal Bounds.SphereRadius 대응).
        Vec3d lo = candidates[0];
        Vec3d hi = candidates[0];
        for (const Vec3d& v : candidates)
        {
            lo = { std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z) };
            hi = { std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z) };
        }
        const Vec3d center = (lo + hi) * 0.5;
        double radiusSq = 0.0;
        for (const Vec3d& v : candidates)
        {
            radiusSq = std::max(radiusSq, Dot(v - center, v - center));
        }

        // 후보 정점으로 snap하며 hull을 만든다.
        ConvexElem elem;
        if (!elem.HullFromPlanes(planes, candidates, KDopSnapDistSq(std::sqrt(radiusSq))))
        {
            return false;
        }

        // 성공한 요소를 결과에 담는다.
        out.ConvexElems.push_back(elem);
        return true;
    }
}
