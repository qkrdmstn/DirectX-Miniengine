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

    // 상자 중심의 부호 있는 거리와 법선 방향 반크기 투영을 비교해 걸침 여부를 정한다.
    int BoxPlaneCompare(const Box& box, const Plane& plane)
    {
        // 상자 중심과 반크기를 구한다.
        const Vec3d center = (box.Min + box.Max) * 0.5;
        const Vec3d half = (box.Max - box.Min) * 0.5;

        // 평면 법선 방향으로 상자가 뻗는 길이(BoxPushOut)를 구한다.
        const double pushOut = std::fabs(plane.Normal.x) * half.x
            + std::fabs(plane.Normal.y) * half.y
            + std::fabs(plane.Normal.z) * half.z;

        // 중심 거리가 투영 길이보다 크면 앞, 작으면 뒤, 아니면 걸침이다.
        const double dist = plane.PlaneDot(center);
        if (dist > pushOut)
        {
            return 1;
        }
        if (dist < -pushOut)
        {
            return -1;
        }
        return 0;
    }

    // 요소의 지지 평면에 뒤집은 절단 평면을 더해 양의 쪽 hull을 다시 만든다.
    bool SliceConvexElem(const ConvexElem& in, const Plane& slicePlane, ConvexElem& out)
    {
        // 지지 평면이 4개 미만이면 절단할 수 없다.
        std::vector<Plane> planes;
        in.GetPlanes(planes);
        if (planes.size() < 4)
        {
            out.Reset();
            return false;
        }

        // 뒤집은 평면을 더해 양의 쪽만 안쪽이 되게 한다.
        planes.push_back({ slicePlane.Normal * -1.0, -slicePlane.W });

        // 원래 정점을 snap 후보로 hull을 만들고 정점 4개 미만이면 실패다.
        if (!out.HullFromPlanes(planes, in.VertexData) || out.VertexData.size() < 4)
        {
            out.Reset();
            return false;
        }
        return true;
    }

    // 요소마다 상자 판정으로 통째 전달하거나 걸치면 양쪽을 각각 잘라 담는다.
    void SliceAggregateGeom(const AggregateGeom& in, const Plane& slicePlane, AggregateGeom& keep, AggregateGeom& other)
    {
        // 이전 결과를 비우고 음의 쪽 절단에 쓸 뒤집은 평면을 만든다.
        keep.ConvexElems.clear();
        other.ConvexElems.clear();
        const Plane flipped = { slicePlane.Normal * -1.0, -slicePlane.W };

        // 요소마다 상자 판정에 따라 통째 전달하거나 양쪽을 각각 자른다.
        for (const ConvexElem& elem : in.ConvexElems)
        {
            const int side = BoxPlaneCompare(elem.ElemBox, slicePlane);
            if (side > 0)
            {
                keep.ConvexElems.push_back(elem);
            }
            else if (side < 0)
            {
                other.ConvexElems.push_back(elem);
            }
            else
            {
                ConvexElem sliced;
                if (SliceConvexElem(elem, slicePlane, sliced))
                {
                    keep.ConvexElems.push_back(sliced);
                }
                if (SliceConvexElem(elem, flipped, sliced))
                {
                    other.ConvexElems.push_back(sliced);
                }
            }
        }
    }
}
