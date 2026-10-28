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

    namespace
    {
        // 벡터 세 성분이 모두 유한한지 검사한다.
        bool IsFiniteVec(const Vec3d& v)
        {
            // 세 성분이 모두 유한해야 참이다.
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // DOP 값이 모두 유한하고 축마다 Min <= Max인지 검사한다.
        bool IsValidDop(const Dop18Bounds& dop)
        {
            // 축마다 유한성과 순서를 확인한다.
            for (int k = 0; k < 9; ++k)
            {
                if (!std::isfinite(dop.Min[k]) || !std::isfinite(dop.Max[k]) || dop.Min[k] > dop.Max[k])
                {
                    return false;
                }
            }
            return true;
        }

        // 한 축에서 모든 정점이 DOP 범위 밖(margin 초과)이면 확실히 겹치지 않는다.
        bool IsCertainlyDisjoint(const ConvexElem& elem, const Dop18Bounds& dop, double margin)
        {
            // 축마다 정점 투영의 최소·최대를 구해 범위 밖인지 본다.
            for (int k = 0; k < 9; ++k)
            {
                double lo = Dot(KDopDirs18[2 * k], elem.VertexData[0]);
                double hi = lo;
                for (const Vec3d& v : elem.VertexData)
                {
                    const double d = Dot(KDopDirs18[2 * k], v);
                    lo = std::min(lo, d);
                    hi = std::max(hi, d);
                }
                if (lo > dop.Max[k] + margin || hi < dop.Min[k] - margin)
                {
                    return true;
                }
            }
            return false;
        }

        // 정확한 교집합 다면체 정점이 재맞춤 요소의 모든 면 안(허용 오차 내)에 있는지 검사한다.
        bool ContainsAll(const std::vector<Vec3d>& points, const ConvexElem& refit, double tol)
        {
            // 재맞춤 요소의 지지 평면을 얻는다.
            std::vector<Plane> planes;
            refit.GetPlanes(planes);
            if (planes.size() < 4)
            {
                return false;
            }

            // 모든 점이 모든 평면의 안쪽(PlaneDot <= tol)인지 확인한다.
            for (const Vec3d& p : points)
            {
                for (const Plane& plane : planes)
                {
                    if (plane.PlaneDot(p) > tol)
                    {
                        return false;
                    }
                }
            }
            return true;
        }
    }

    // 원본 면 평면과 pad를 더한 DOP 평면의 교집합을 만들고 포함·부피 검증을 통과할 때만 채택한다.
    RefitOutcome RefitElemToDop(const ConvexElem& elem, const Dop18Bounds& dop, double pad, ConvexElem& out)
    {
        // 기본 결과를 원본 복사로 두고 입력을 검증한다.
        out = elem;
        bool valid = std::isfinite(pad) && pad >= 0.0 && IsValidDop(dop) && elem.VertexData.size() >= 4;
        double scale = 0.0;
        for (const Vec3d& v : elem.VertexData)
        {
            valid = valid && IsFiniteVec(v);
            scale = std::max(scale, std::max(std::fabs(v.x), std::max(std::fabs(v.y), std::fabs(v.z))));
        }
        if (!valid)
        {
            return RefitOutcome::KeptUnrefit;
        }

        // 다각형은 원점 기준의 유한 사각형에서 시작하므로 요소와 패딩된 DOP 좌표를 절반 범위 안으로 제한한다.
        // 이 여유가 없으면 실제 교집합 꼭짓점이 후보와 검증에서 함께 잘릴 수 있어 원본을 유지한다.
        double maxCoordinate = scale;
        for (int k = 0; k < 3; ++k)
        {
            const double lo = dop.Min[k] - pad;
            const double hi = dop.Max[k] + pad;
            if (!std::isfinite(lo) || !std::isfinite(hi))
            {
                return RefitOutcome::KeptUnrefit;
            }
            maxCoordinate = std::max(maxCoordinate, std::max(std::fabs(lo), std::fabs(hi)));
        }
        if (maxCoordinate > 0.5 * BigPolygonHalfExtent)
        {
            return RefitOutcome::KeptUnrefit;
        }

        // 한 축에서라도 모두 범위 밖이면 겹치지 않는다고 확정한다.
        if (IsCertainlyDisjoint(elem, dop, pad))
        {
            return RefitOutcome::Disjoint;
        }

        // 원본 면 평면에 pad를 더한 DOP 평면 18개를 합친다.
        std::vector<Plane> planes;
        elem.GetPlanes(planes);
        if (planes.size() < 4)
        {
            return RefitOutcome::KeptUnrefit;
        }
        for (int k = 0; k < 9; ++k)
        {
            planes.push_back({ KDopDirs18[2 * k], dop.Max[k] + pad });
            planes.push_back({ KDopDirs18[2 * k] * -1.0, -(dop.Min[k] - pad) });
        }

        // 병합 전 정확한 교집합 정점을 모으고, 병합·snap이 반영된 임시 hull을 만든다.
        std::vector<Vec3d> exact;
        CollectPlanePolygonVertices(planes, HullTolerances(), exact);
        ConvexElem refit;
        if (exact.size() < 4 || !refit.HullFromPlanes(planes, {}))
        {
            return RefitOutcome::KeptUnrefit;
        }

        // 임시 hull이 정확한 교집합을 포함하고 원본보다 커지지 않을 때만 채택한다.
        const double acceptTol = 1e-9 * scale + 1e-12;
        const double volume = elem.GetVolume();
        if (!ContainsAll(exact, refit, acceptTol) || refit.GetVolume() > volume * (1.0 + 1e-9) + 1e-18)
        {
            return RefitOutcome::KeptUnrefit;
        }
        out = refit;
        return RefitOutcome::Refit;
    }

    // 요소마다 재맞춤하고, 하나라도 남는 경우에만 Disjoint 요소를 버린다.
    RefitStats RefitAggregateToDop(const AggregateGeom& in, const Dop18Bounds& dop, double pad, AggregateGeom& out)
    {
        // 요소마다 재맞춤해 결과와 집계 전 부피를 모은다.
        RefitStats stats;
        std::vector<ConvexElem> results;
        std::vector<RefitOutcome> outcomes;
        for (const ConvexElem& elem : in.ConvexElems)
        {
            ConvexElem refit;
            outcomes.push_back(RefitElemToDop(elem, dop, pad, refit));
            results.push_back(refit);
            stats.VolumeBefore += elem.GetVolume();
        }

        // 남는 요소가 하나도 없게 되면 Disjoint 요소를 원본으로 유지한다.
        bool anyKept = false;
        for (RefitOutcome outcome : outcomes)
        {
            anyKept = anyKept || outcome != RefitOutcome::Disjoint;
        }

        // 결과를 out에 모으며 개수와 부피를 집계한다.
        out.ConvexElems.clear();
        for (size_t i = 0; i < results.size(); ++i)
        {
            if (outcomes[i] == RefitOutcome::Disjoint && anyKept)
            {
                ++stats.Disjoint;
                continue;
            }
            if (outcomes[i] == RefitOutcome::Refit)
            {
                ++stats.Refit;
            }
            else
            {
                ++stats.Kept;
            }
            stats.VolumeAfter += results[i].GetVolume();
            out.ConvexElems.push_back(results[i]);
        }
        return stats;
    }
}
