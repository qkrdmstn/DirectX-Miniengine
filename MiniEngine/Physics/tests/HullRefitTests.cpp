#include "CollisionTestUtils.h"
#include "Collision/AggregateGeom.h"
#include <algorithm>
#include <cstdio>
#include <limits>

// 중심과 반크기로 축 정렬 상자 hull 요소를 만든다.
static ConvexElem MakeBox(Vec3d c, Vec3d half)
{
    // 상자 평면의 교집합으로 hull을 만들고 성공을 확인한다.
    ConvexElem e;
    Check(e.HullFromPlanes(AlignedBox(c, half), {}), "box hull");
    return e;
}

// 상자의 여덟 모서리 점을 돌려준다.
static std::vector<Vec3d> BoxCorners(Vec3d c, Vec3d half)
{
    // 부호 조합 여덟 가지로 모서리를 만든다.
    std::vector<Vec3d> pts;
    for (int i = 0; i < 8; ++i)
    {
        pts.push_back({ c.x + ((i & 1) ? half.x : -half.x), c.y + ((i & 2) ? half.y : -half.y), c.z + ((i & 4) ? half.z : -half.z) });
    }
    return pts;
}

// 점 집합의 18-DOP 아홉 축(KDopDirs18 짝수 인덱스) 투영 범위를 측정한다.
static Dop18Bounds MeasureDop(const std::vector<Vec3d>& pts)
{
    // 축 아홉 개를 KDopDirs18의 짝수 인덱스와 같은 순서로 둔다.
    const double r = 0.70710678118654752440;
    const Vec3d axes[9] = { {1,0,0}, {0,1,0}, {0,0,1}, {0,r,r}, {0,r,-r}, {r,0,r}, {r,0,-r}, {r,r,0}, {r,-r,0} };

    // 축마다 투영의 최소·최대를 구한다.
    Dop18Bounds dop{};
    for (int k = 0; k < 9; ++k)
    {
        dop.Min[k] = Dot(axes[k], pts[0]);
        dop.Max[k] = dop.Min[k];
        for (const Vec3d& p : pts)
        {
            dop.Min[k] = std::min(dop.Min[k], Dot(axes[k], p));
            dop.Max[k] = std::max(dop.Max[k], Dot(axes[k], p));
        }
    }
    return dop;
}

// 점이 요소의 모든 지지 평면 안쪽(허용 오차 내)에 있는지 검사한다.
static bool Contains(const ConvexElem& e, Vec3d p, double tol)
{
    // 지지 평면마다 PlaneDot이 tol 이하인지 확인한다.
    std::vector<Plane> planes;
    e.GetPlanes(planes);
    for (const Plane& plane : planes)
    {
        if (plane.PlaneDot(p) > tol)
        {
            return false;
        }
    }
    return !planes.empty();
}

// 점 집합이 모두 요소 안에 있는지 검사한다.
static bool ContainsAll(const ConvexElem& e, const std::vector<Vec3d>& pts, double tol)
{
    // 점마다 포함 여부를 확인한다.
    for (const Vec3d& p : pts)
    {
        if (!Contains(e, p, tol))
        {
            return false;
        }
    }
    return true;
}

// 평면 삼중쌍의 연립방정식을 직접 풀어 클리핑과 무관하게 교집합 꼭짓점을 모은다.
static std::vector<Vec3d> ExactIntersectionVertices(const ConvexElem& elem, const Dop18Bounds& dop, double pad)
{
    // 원본 면과 패딩된 DOP 면을 같은 반공간 부호로 모은다.
    std::vector<Plane> planes;
    elem.GetPlanes(planes);
    const double r = 0.70710678118654752440;
    const Vec3d axes[9] = { {1,0,0}, {0,1,0}, {0,0,1}, {0,r,r}, {0,r,-r}, {r,0,r}, {r,0,-r}, {r,r,0}, {r,-r,0} };
    for (int k = 0; k < 9; ++k)
    {
        planes.push_back({ axes[k], dop.Max[k] + pad });
        planes.push_back({ axes[k] * -1.0, -dop.Min[k] + pad });
    }

    // 독립인 세 법선마다 크래머 공식으로 교점을 계산한다.
    std::vector<Vec3d> vertices;
    for (size_t i = 0; i < planes.size(); ++i)
    {
        for (size_t j = i + 1; j < planes.size(); ++j)
        {
            for (size_t k = j + 1; k < planes.size(); ++k)
            {
                const Vec3d cross = Cross(planes[j].Normal, planes[k].Normal);
                const double det = Dot(planes[i].Normal, cross);
                if (std::fabs(det) <= 1e-12)
                {
                    continue;
                }
                const Vec3d point = (cross * planes[i].W
                    + Cross(planes[k].Normal, planes[i].Normal) * planes[j].W
                    + Cross(planes[i].Normal, planes[j].Normal) * planes[k].W) * (1.0 / det);

                // 모든 반공간을 절대 거리 1e-9 안에서 만족하는 교점만 유지한다.
                bool inside = true;
                for (const Plane& plane : planes)
                {
                    inside = inside && plane.PlaneDot(point) <= 1e-9;
                }
                if (inside)
                {
                    vertices.push_back(point);
                }
            }
        }
    }
    return vertices;
}

// 독립 계산한 교집합의 각 꼭짓점이 결과의 모든 면 안에 있는지 검사한다.
static void CheckIntersectionContained(const ConvexElem& elem, const Dop18Bounds& dop, double pad, const ConvexElem& out)
{
    // 빈 정점 집합이나 면 집합으로 검사가 무의미하게 통과하지 않도록 확인한다.
    const std::vector<Vec3d> vertices = ExactIntersectionVertices(elem, dop, pad);
    std::vector<Plane> planes;
    out.GetPlanes(planes);
    Check(vertices.size() >= 4, "independent intersection vertices");
    Check(planes.size() >= 4, "result face planes");

    // 부피 대신 각 교집합 꼭짓점과 결과 면 사이의 부호 있는 거리를 검사한다.
    for (const Vec3d& vertex : vertices)
    {
        for (const Plane& plane : planes)
        {
            Check(plane.PlaneDot(vertex) <= 1e-9, "independent intersection containment");
        }
    }
}

// 모든 단일 요소 테스트에서 Refit 결과의 포함성을 독립적으로 검증한다.
static RefitOutcome CheckedRefit(const ConvexElem& elem, const Dop18Bounds& dop, double pad, ConvexElem& out)
{
    // 입력을 보존한 상태에서 재맞춤 결과를 얻는다.
    const RefitOutcome outcome = RefitElemToDop(elem, dop, pad, out);

    // 채택된 후보는 정확한 교집합의 모든 꼭짓점을 포함해야 한다.
    if (outcome == RefitOutcome::Refit)
    {
        CheckIntersectionContained(elem, dop, pad, out);
    }
    return outcome;
}

// 상자를 x = 0.3 평면의 양의 쪽으로 잘라 조각 요소를 돌려준다.
static ConvexElem SliceBoxOffCenter()
{
    // 법선 +x, W = 0.3 평면으로 자른다.
    ConvexElem piece;
    Check(SliceConvexElem(MakeBox({ 0, 0, 0 }, { 1, 1, 1 }), { { 1, 0, 0 }, 0.3 }, piece), "slice off-center");
    return piece;
}

// 조각의 DOP으로 재맞춤하면 조각 상자(부피 2.8)와 같아야 한다.
static void TestOffCenterBox()
{
    // 잘린 조각을 자기 DOP과 pad로 재맞춤해도 부피가 유지된다.
    const ConvexElem piece = SliceBoxOffCenter();
    const Dop18Bounds dop = MeasureDop(piece.VertexData);
    ConvexElem out;
    Check(CheckedRefit(piece, dop, RefitDopPad, out) == RefitOutcome::Refit, "piece refit");
    CheckNear(out.GetVolume(), 2.8, 2.8 * 1e-6, "piece volume");

    // 잘리기 전 원본 상자를 DOP으로 재맞춤(pad 1e-7)하면 조각 상자가 된다.
    ConvexElem fromWhole;
    const RefitOutcome w = CheckedRefit(MakeBox({ 0, 0, 0 }, { 1, 1, 1 }), dop, 1e-7, fromWhole);
    std::printf("whole outcome %d\n", static_cast<int>(w));
    Check(w == RefitOutcome::Refit, "whole refit");
    CheckNear(fromWhole.GetVolume(), 2.8, 2.8 * 1e-6, "whole to piece volume");
    std::printf("off-center box: piece %.9f, whole->piece %.9f\n", out.GetVolume(), fromWhole.GetVolume());
}

// 큰 18-DOP hull을 작은 하위 상자 DOP으로 줄이면 부피가 크게 줄고 모서리를 포함한다.
static void TestEarSlab()
{
    // 큰 hull을 만들고 모서리 근처 작은 상자의 DOP을 잰다.
    AggregateGeom big;
    Check(GenerateKDop18(BoxCorners({ 0, 0, 0 }, { 1, 1, 1 }), big), "big dop hull");
    const std::vector<Vec3d> small = BoxCorners({ 0.8, 0.8, 0.8 }, { 0.1, 0.1, 0.1 });
    const Dop18Bounds dop = MeasureDop(small);

    // 재맞춤하고 부피 감소와 모서리 포함을 확인한다.
    ConvexElem out;
    Check(CheckedRefit(big.ConvexElems[0], dop, RefitDopPad, out) == RefitOutcome::Refit, "ear refit");
    const double before = big.ConvexElems[0].GetVolume();
    const double after = out.GetVolume();
    Check(after < before / 100.0, "ear volume reduction");
    Check(ContainsAll(out, small, 1e-9), "ear contains corners");

    // 같은 DOP으로 한 번 더 재맞춤해도 부피가 같다(멱등).
    ConvexElem again;
    CheckedRefit(out, dop, RefitDopPad, again);
    CheckNear(again.GetVolume(), after, after * 1e-9, "idempotent volume");
    std::printf("ear slab: %.6f -> %.6f (x%.1f reduction), second pass %.6f\n", before, after, before / after, again.GetVolume());
}

// 한 요소가 DOP과 멀면 Disjoint로 버려지고 나머지는 재맞춤된다.
static void TestDisjointDropped()
{
    // 원점 상자와 멀리 있는 상자를 묶고 원점 상자의 DOP을 쓴다.
    AggregateGeom in;
    in.ConvexElems.push_back(MakeBox({ 0, 0, 0 }, { 1, 1, 1 }));
    in.ConvexElems.push_back(MakeBox({ 10, 0, 0 }, { 1, 1, 1 }));
    const Dop18Bounds dop = MeasureDop(BoxCorners({ 0, 0, 0 }, { 1, 1, 1 }));

    // 먼 요소만 사라지고 나머지는 재맞춤이다.
    AggregateGeom out;
    const RefitStats s = RefitAggregateToDop(in, dop, RefitDopPad, out);
    Check(s.Refit == 1 && s.Disjoint == 1 && s.Kept == 0, "one refit one disjoint");
    Check(out.ConvexElems.size() == 1, "disjoint dropped");
    CheckIntersectionContained(in.ConvexElems[0], dop, RefitDopPad, out.ConvexElems[0]);
    CheckNear(s.VolumeBefore, 16.0, 1e-9, "volume before");
    CheckNear(s.VolumeAfter, 8.0, 8e-6, "volume after");
}

// 모든 요소가 Disjoint면 전부 원본으로 유지하고 Kept로 센다.
static void TestAllDisjointKept()
{
    // DOP을 두 상자와 모두 먼 곳에 둔다.
    AggregateGeom in;
    in.ConvexElems.push_back(MakeBox({ 0, 0, 0 }, { 1, 1, 1 }));
    in.ConvexElems.push_back(MakeBox({ 5, 0, 0 }, { 1, 1, 1 }));
    const Dop18Bounds dop = MeasureDop(BoxCorners({ 0, 20, 0 }, { 1, 1, 1 }));

    // 요소가 모두 남고 Kept 개수가 요소 수와 같다.
    AggregateGeom out;
    const RefitStats s = RefitAggregateToDop(in, dop, RefitDopPad, out);
    Check(s.Kept == 2 && s.Refit == 0 && s.Disjoint == 0, "all kept");
    Check(out.ConvexElems.size() == 2, "all elems retained");
    CheckNear(s.VolumeAfter, s.VolumeBefore, 1e-12, "volume unchanged");
}

// 얇은 조각은 Kept이거나 Refit이어도 실제 조각 모서리를 포함한다.
static void TestThinSliver()
{
    // 두께 0.0015 판의 모서리 DOP으로 재맞춤한다.
    const Vec3d half = { 1, 1, 0.00075 };
    const ConvexElem sliver = MakeBox({ 0, 0, 0 }, half);
    const std::vector<Vec3d> corners = BoxCorners({ 0, 0, 0 }, half);
    ConvexElem out;
    const RefitOutcome r = CheckedRefit(sliver, MeasureDop(corners), RefitDopPad, out);

    // 어느 결과든 실제 모서리를 포함해야 한다.
    Check(r != RefitOutcome::Disjoint, "sliver not disjoint");
    Check(ContainsAll(out, corners, 1e-9), "sliver contains corners");
    std::printf("thin sliver: outcome %d, volume %.9f (orig %.9f)\n", static_cast<int>(r), out.GetVolume(), sliver.GetVolume());
}

// DOP 평면이 모서리를 병합 거리보다 얇게 깎아도 Kept이거나 교집합을 포함하는 Refit이다.
static void TestNearMergeCorner()
{
    // 대각 축(1,1,0)의 최댓값을 모서리 위로 0.0004만 깎는다(pad 0).
    const ConvexElem box = MakeBox({ 0, 0, 0 }, { 1, 1, 1 });
    Dop18Bounds dop = MeasureDop(BoxCorners({ 0, 0, 0 }, { 1, 1, 1 }));
    dop.Max[7] -= 0.0004;
    ConvexElem out;
    const RefitOutcome r = CheckedRefit(box, dop, 0.0, out);

    // 병합된 후보가 실제로 생성되지만 정확한 교집합을 포함하지 못하는 상황을 재현한다.
    std::vector<Plane> planes;
    box.GetPlanes(planes);
    const double axisRcp = 0.70710678118654752440;
    const Vec3d axes[9] = { {1,0,0}, {0,1,0}, {0,0,1}, {0,axisRcp,axisRcp}, {0,axisRcp,-axisRcp},
        {axisRcp,0,axisRcp}, {axisRcp,0,-axisRcp}, {axisRcp,axisRcp,0}, {axisRcp,-axisRcp,0} };
    for (int k = 0; k < 9; ++k)
    {
        planes.push_back({ axes[k], dop.Max[k] });
        planes.push_back({ axes[k] * -1.0, -dop.Min[k] });
    }
    ConvexElem candidate;
    Check(candidate.HullFromPlanes(planes, {}), "near-merge candidate built");
    Check(!ContainsAll(candidate, ExactIntersectionVertices(box, dop, 0.0), 1e-9), "near-merge candidate loses corner");

    // 포함 검증에서 후보를 거부하고 원본을 돌려주는지 확인한다.
    Check(r == RefitOutcome::KeptUnrefit, "near-merge candidate rejected");
    Check(out.IndexData == box.IndexData, "rejected candidate indices copied");
    CheckNear(out.GetVolume(), 8.0, 1e-5, "near-merge volume");
    CheckIntersectionContained(box, dop, 0.0, out);
    Check(out.GetVolume() <= 8.0 * (1.0 + 1e-9), "near-merge not larger");
    std::printf("near-merge corner: outcome %d, volume %.9f\n", static_cast<int>(r), out.GetVolume());
}

// DOP이 요소보다 크면 결과는 요소와 같다.
static void TestLargerDop()
{
    // 두 배 큰 상자의 DOP으로 재맞춤한다.
    const ConvexElem box = MakeBox({ 0, 0, 0 }, { 1, 1, 1 });
    ConvexElem out;
    const RefitOutcome r = CheckedRefit(box, MeasureDop(BoxCorners({ 0, 0, 0 }, { 2, 2, 2 })), RefitDopPad, out);
    Check(r == RefitOutcome::Refit, "larger dop refit");
    CheckNear(out.GetVolume(), box.GetVolume(), 8.0 * 1e-9, "larger dop volume");
}

// 중복 공면이 있어도 교집합을 포함하거나 원본을 유지해야 한다.
static void TestCoplanarDuplicates()
{
    // 패딩 후 x 최대 면이 원본 면과 정확히 겹치도록 두 패딩 값을 검사한다.
    const ConvexElem box = MakeBox({ 0, 0, 0 }, { 1, 1, 1 });
    const double pads[2] = { 0.0, RefitDopPad };
    for (double pad : pads)
    {
        Dop18Bounds dop = MeasureDop(box.VertexData);
        dop.Max[0] -= pad;
        ConvexElem out;
        const RefitOutcome outcome = CheckedRefit(box, dop, pad, out);
        Check(outcome == RefitOutcome::Refit || outcome == RefitOutcome::KeptUnrefit, "coplanar outcome");
        CheckIntersectionContained(box, dop, pad, out);
    }
}

// 큰 좌표는 원본을 보존하고 안전 범위의 큰 상자는 정상적으로 재맞춤한다.
static void TestCoordinateGuard()
{
    // 고정 사각형의 영향을 피하도록 기존 상자 정점을 직접 확대한다.
    ConvexElem large = MakeBox({ 0, 0, 0 }, { 1, 1, 1 });
    for (Vec3d& vertex : large.VertexData)
    {
        vertex = vertex * 20000.0;
    }
    large.UpdateElemBox();
    ConvexElem out;
    Check(CheckedRefit(large, MeasureDop(large.VertexData), RefitDopPad, out) == RefitOutcome::KeptUnrefit, "large coordinate guard");
    Check(out.IndexData == large.IndexData, "large indices copied");
    Check(out.VertexData.size() == large.VertexData.size(), "large vertices copied");
    for (size_t i = 0; i < large.VertexData.size(); ++i)
    {
        Check(Dot(out.VertexData[i] - large.VertexData[i], out.VertexData[i] - large.VertexData[i]) == 0.0, "large coordinate copied");
    }

    // 요소 자체가 작아도 패딩된 DOP 좌표가 범위를 넘으면 원본을 유지한다.
    const ConvexElem box = MakeBox({ 0, 0, 0 }, { 1, 1, 1 });
    const Dop18Bounds hugeDop = MeasureDop(large.VertexData);
    Check(CheckedRefit(box, hugeDop, 0.0, out) == RefitOutcome::KeptUnrefit, "large dop guard");
    const Dop18Bounds smallDop = MeasureDop(box.VertexData);
    Check(CheckedRefit(box, smallDop, 6000.0, out) == RefitOutcome::KeptUnrefit, "padded dop guard");

    // 반크기 4000인 상자와 원점에서 4000만큼 이동한 상자는 안전 범위 안이다.
    const ConvexElem safe = MakeBox({ 0, 0, 0 }, { 4000, 4000, 4000 });
    Check(CheckedRefit(safe, MeasureDop(safe.VertexData), RefitDopPad, out) == RefitOutcome::Refit, "safe large box refit");
    const ConvexElem shifted = MakeBox({ 4000, 0, 0 }, { 1, 1, 1 });
    Check(CheckedRefit(shifted, MeasureDop(shifted.VertexData), RefitDopPad, out) == RefitOutcome::Refit, "safe shifted box refit");
}

// 잘못된 입력은 모두 Kept이고 out은 요소 복사본이다.
static void TestInvalidInputs()
{
    // NaN이 든 DOP과 Min > Max인 DOP은 거부한다.
    const ConvexElem box = MakeBox({ 0, 0, 0 }, { 1, 1, 1 });
    const Dop18Bounds good = MeasureDop(BoxCorners({ 0, 0, 0 }, { 1, 1, 1 }));
    ConvexElem out;
    Dop18Bounds nan = good;
    nan.Max[3] = std::numeric_limits<double>::quiet_NaN();
    Check(CheckedRefit(box, nan, RefitDopPad, out) == RefitOutcome::KeptUnrefit, "nan dop");
    Check(out.VertexData.size() == box.VertexData.size(), "nan dop copy");
    Dop18Bounds inverted = good;
    std::swap(inverted.Min[0], inverted.Max[0]);
    Check(CheckedRefit(box, inverted, RefitDopPad, out) == RefitOutcome::KeptUnrefit, "min > max");

    // 빈 요소와 NaN 정점 요소도 거부한다.
    ConvexElem empty;
    Check(CheckedRefit(empty, good, RefitDopPad, out) == RefitOutcome::KeptUnrefit, "empty elem");
    Check(out.VertexData.empty(), "empty elem copy");
    ConvexElem bad = box;
    bad.VertexData[0].x = std::numeric_limits<double>::quiet_NaN();
    Check(CheckedRefit(bad, good, RefitDopPad, out) == RefitOutcome::KeptUnrefit, "nan vertex");
}

// 모든 테스트를 실행하고 검사 횟수를 출력한다.
int main()
{
    // 모든 테스트를 순서대로 실행하고 예외가 나면 실패로 끝낸다.
    try
    {
        TestOffCenterBox();
        TestEarSlab();
        TestDisjointDropped();
        TestAllDisjointKept();
        TestThinSliver();
        TestNearMergeCorner();
        TestLargerDop();
        TestCoplanarDuplicates();
        TestCoordinateGuard();
        TestInvalidInputs();
    }
    catch (const std::exception& e)
    {
        // 실패 메시지와 지금까지의 검사 횟수를 출력한다.
        std::printf("FAILED: %s (after %zu checks)\n", e.what(), g_Checks);
        return 1;
    }

    // 통과한 검사 횟수를 출력한다.
    std::printf("HullRefitTests passed: %zu checks\n", g_Checks);
    return 0;
}
