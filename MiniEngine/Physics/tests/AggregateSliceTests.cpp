#include "CollisionTestUtils.h"
#include "Collision/AggregateGeom.h"
#include <algorithm>
#include <cstdio>

// 중심과 반크기로 축 정렬 상자 hull 요소를 만든다.
static ConvexElem MakeBox(Vec3d c, Vec3d half)
{
    // 상자 평면의 교집합으로 hull을 만들고 성공을 확인한다.
    ConvexElem e;
    Check(e.HullFromPlanes(AlignedBox(c, half), {}), "box hull");
    return e;
}

// 요소 하나짜리 묶음을 만든다.
static AggregateGeom Wrap(const ConvexElem& e)
{
    // 요소를 목록에 넣어 돌려준다.
    AggregateGeom g;
    g.ConvexElems.push_back(e);
    return g;
}

// 묶음의 모든 요소 부피 합을 돌려준다.
static double TotalVolume(const AggregateGeom& g)
{
    // 요소마다 부피를 더한다.
    double v = 0.0;
    for (const ConvexElem& e : g.ConvexElems)
    {
        v += e.GetVolume();
    }
    return v;
}

// 묶음의 모든 정점이 평면 기준 부호 방향에 허용 오차 안으로 있는지 검사한다.
static bool AllOnSide(const AggregateGeom& g, const Plane& plane, double sign, double tol)
{
    // 정점마다 부호를 곱한 PlaneDot이 -tol 이상인지 확인한다.
    for (const ConvexElem& e : g.ConvexElems)
    {
        for (const Vec3d& v : e.VertexData)
        {
            if (sign * plane.PlaneDot(v) < -tol)
            {
                return false;
            }
        }
    }
    return true;
}

// BoxPlaneCompare가 앞(+1)·뒤(-1)·걸침(0)을 구분하는지 검사한다.
static void TestBoxPlaneCompare()
{
    // 상자 [-1,1]^3과 y축 법선 평면의 위치별 결과를 확인한다.
    const ConvexElem box = MakeBox({ 0, 0, 0 }, { 1, 1, 1 });
    Check(BoxPlaneCompare(box.ElemBox, { { 0, 1, 0 }, -2.0 }) == 1, "all front");
    Check(BoxPlaneCompare(box.ElemBox, { { 0, 1, 0 }, 2.0 }) == -1, "all behind");
    Check(BoxPlaneCompare(box.ElemBox, { { 0, 1, 0 }, 0.0 }) == 0, "straddle");

    // 면에 닿는 평면은 허용 오차 없이 걸침이고, 대각 법선은 투영 길이로 판정한다.
    // 실제 hull은 정점 병합과 snap으로 경계 상자가 조금 달라질 수 있어 닿는 경우는 상자 값을 직접 쓴다.
    Box exact;
    exact.Min = { -1, -1, -1 };
    exact.Max = { 1, 1, 1 };
    exact.IsValid = true;
    Check(BoxPlaneCompare(exact, { { 0, 1, 0 }, 1.0 }) == 0, "touching face");
    Check(BoxPlaneCompare(exact, { { 0, 1, 0 }, -1.0 }) == 0, "touching back face");
    const double r = std::sqrt(1.0 / 3.0);
    Check(BoxPlaneCompare(exact, { { r, r, r }, -3.0 * r - 0.01 }) == 1, "diagonal front");
    Check(BoxPlaneCompare(exact, { { r, r, r }, 3.0 * r - 0.01 }) == 0, "diagonal corner straddle");
}

// 정육면체를 y=0으로 잘라 두 쪽 부피와 합을 검사한다.
static void TestCubeSlice()
{
    // 일반 상자 hull은 합이 부모와 1e-9 상대오차로 같다.
    const Plane plane = { { 0, 1, 0 }, 0.0 };
    const AggregateGeom parent = Wrap(MakeBox({ 0, 0, 0 }, { 1, 1, 1 }));
    AggregateGeom keep;
    AggregateGeom other;
    SliceAggregateGeom(parent, plane, keep, other);
    Check(keep.ConvexElems.size() == 1 && other.ConvexElems.size() == 1, "cube pieces");
    CheckNear(TotalVolume(keep), 4.0, 4e-9, "keep half");
    CheckNear(TotalVolume(other), 4.0, 4e-9, "other half");
    CheckNear(TotalVolume(keep) + TotalVolume(other), 8.0, 8e-9, "sum equals parent");
    Check(AllOnSide(keep, plane, 1.0, 1e-9) && AllOnSide(other, plane, -1.0, 1e-9), "sides");
    Check(IsClosedSurface(keep.ConvexElems[0]), "keep closed");

    // 18-DOP 부모는 정점 병합 오차를 고려해 1e-6 상대오차로 합이 같다.
    AggregateGeom dop;
    const std::vector<Vec3d> pts = { {-1,-1,-1}, {1,-1,-1}, {-1,1,-1}, {1,1,-1}, {-1,-1,1}, {1,-1,1}, {-1,1,1}, {1,1,1} };
    Check(GenerateKDop18(pts, dop), "dop");
    SliceAggregateGeom(dop, plane, keep, other);
    Check(keep.ConvexElems.size() == 1 && other.ConvexElems.size() == 1, "dop pieces");
    const double parentVolume = TotalVolume(dop);
    CheckNear(TotalVolume(keep) + TotalVolume(other), parentVolume, parentVolume * 1e-6, "dop sum");
}

// 정육면체 18-DOP을 45도로 자를 때 정점 병합에 따른 hull 초과량을 검사한다.
static void TestCubeDiagonalHullContainment()
{
    // 정육면체 18-DOP을 원점을 지나는 45도 평면으로 자른다.
    const std::vector<Vec3d> pts = { {-1,-1,-1}, {1,-1,-1}, {-1,1,-1}, {1,1,-1}, {-1,-1,1}, {1,-1,1}, {-1,1,1}, {1,1,1} };
    AggregateGeom dop;
    Check(GenerateKDop18(pts, dop), "diagonal dop");
    const double r = std::sqrt(0.5);
    const Plane plane = { { -r, r, 0 }, 0.0 };
    AggregateGeom keep;
    AggregateGeom other;
    SliceAggregateGeom(dop, plane, keep, other);
    Check(!keep.ConvexElems.empty() && !other.ConvexElems.empty(), "diagonal pieces");

    // 평면 위 꼭짓점을 양쪽에 포함해 각 쪽 지지 평면의 최대 초과량을 구한다.
    for (int side = 0; side < 2; ++side)
    {
        std::vector<Plane> planes;
        const AggregateGeom& piece = side == 0 ? keep : other;
        piece.ConvexElems[0].GetPlanes(planes);
        double maxExcess = -1.0e30;
        for (const Vec3d& p : pts)
        {
            const double d = plane.PlaneDot(p);
            if ((side == 0 && d >= -1e-9) || (side == 1 && d <= 1e-9))
            {
                for (const Plane& hullPlane : planes)
                {
                    maxExcess = std::max(maxExcess, hullPlane.PlaneDot(p));
                }
            }
        }

        // 병합 근사가 실제로 생기며 병합 거리와 snap 오차의 합 이내인지 확인한다.
        Check(maxExcess > 0.0 && maxExcess <= MinHullVertDistance + 1e-6, "diagonal hull excess");
    }
}

// 한쪽에 통째로 있거나 면과 겹치는 평면을 처리하는지 검사한다.
static void TestPassThroughAndFace()
{
    // 완전히 앞에 있는 요소는 그대로 keep에, 다른 쪽은 빈다.
    const ConvexElem front = MakeBox({ 0, 3, 0 }, { 1, 1, 1 });
    AggregateGeom keep;
    AggregateGeom other;
    SliceAggregateGeom(Wrap(front), { { 0, 1, 0 }, 0.0 }, keep, other);
    Check(keep.ConvexElems.size() == 1 && other.ConvexElems.empty(), "front pass");
    Check(keep.ConvexElems[0].VertexData.size() == front.VertexData.size(), "front unchanged");
    CheckNear(TotalVolume(keep), front.GetVolume(), 0.0, "front volume");

    // 완전히 뒤에 있는 요소는 그대로 other에 간다.
    SliceAggregateGeom(Wrap(front), { { 0, -1, 0 }, 0.0 }, keep, other);
    Check(keep.ConvexElems.empty() && other.ConvexElems.size() == 1, "back pass");

    // 윗면과 일치하는 평면은 앞쪽이 비고 뒤쪽은 부피가 유지된다.
    const ConvexElem cube = MakeBox({ 0, 0, 0 }, { 1, 1, 1 });
    SliceAggregateGeom(Wrap(cube), { { 0, 1, 0 }, 1.0 }, keep, other);
    Check(keep.ConvexElems.empty(), "face keep empty");
    Check(other.ConvexElems.size() == 1, "face other kept");
    CheckNear(TotalVolume(other), 8.0, 8e-9, "face other volume");
}

// 모서리 근처 평면이 작은 사면체를 만드는지 검사한다.
static void TestCornerTetra()
{
    // 모서리 (1,1,1)에서 높이 0.5 지점을 지나는 대각 평면으로 자른다.
    const double r = std::sqrt(1.0 / 3.0);
    const Plane plane = { { r, r, r }, 3.0 * r - 0.5 };
    AggregateGeom keep;
    AggregateGeom other;
    SliceAggregateGeom(Wrap(MakeBox({ 0, 0, 0 }, { 1, 1, 1 })), plane, keep, other);

    // 앞쪽은 변 길이 0.5·sqrt(3)인 직각 사면체다.
    Check(keep.ConvexElems.size() == 1 && other.ConvexElems.size() == 1, "corner pieces");
    Check(keep.ConvexElems[0].VertexData.size() == 4, "tetra vertices");
    const double leg = 0.5 * std::sqrt(3.0);
    CheckNear(TotalVolume(keep), leg * leg * leg / 6.0, 1e-9, "tetra volume");
    CheckNear(TotalVolume(keep) + TotalVolume(other), 8.0, 8e-9, "corner sum");
}

// 한 요소만 걸치는 두 요소 묶음의 개수와 부피를 검사한다.
static void TestTwoElems()
{
    // 원점 상자는 걸치고 위쪽 상자는 통째로 앞에 있다.
    AggregateGeom parent = Wrap(MakeBox({ 0, 0, 0 }, { 1, 1, 1 }));
    parent.ConvexElems.push_back(MakeBox({ 0, 3, 0 }, { 1, 1, 1 }));
    AggregateGeom keep;
    AggregateGeom other;
    SliceAggregateGeom(parent, { { 0, 1, 0 }, 0.0 }, keep, other);

    // 앞쪽은 반쪽과 통째 요소 둘, 뒤쪽은 반쪽 하나다.
    Check(keep.ConvexElems.size() == 2 && other.ConvexElems.size() == 1, "two elem counts");
    CheckNear(TotalVolume(keep), 12.0, 1.2e-8, "two elem keep");
    CheckNear(TotalVolume(other), 4.0, 4e-9, "two elem other");
}

// 조각을 다시 절단해도 부피가 보존되는지 검사한다.
static void TestRepeatedSlice()
{
    // 먼저 y=0으로 자른 앞쪽을 다시 x=0으로 자른다.
    AggregateGeom keep;
    AggregateGeom other;
    SliceAggregateGeom(Wrap(MakeBox({ 0, 0, 0 }, { 1, 1, 1 })), { { 0, 1, 0 }, 0.0 }, keep, other);
    AggregateGeom keep2;
    AggregateGeom other2;
    SliceAggregateGeom(keep, { { 1, 0, 0 }, 0.0 }, keep2, other2);

    // 두 조각 모두 부피 2이고 합이 첫 조각과 같다.
    Check(keep2.ConvexElems.size() == 1 && other2.ConvexElems.size() == 1, "repeat counts");
    CheckNear(TotalVolume(keep2), 2.0, 2e-9, "repeat keep");
    CheckNear(TotalVolume(keep2) + TotalVolume(other2), TotalVolume(keep), 4e-9, "repeat sum");
}

// 회전한 원점 밖 상자를 기울어진 평면으로 잘라 조각이 올바른 쪽에 있는지 검사한다.
static void TestRotatedOffCenter()
{
    // 두 번 회전한 직교 축으로 상자 hull을 만든다.
    const double th = 0.5;
    const double ph = 0.3;
    const Vec3d a0 = { std::cos(th), std::sin(th), 0 };
    const Vec3d b1 = { -std::sin(th), std::cos(th), 0 };
    const Vec3d b2 = { 0, 0, 1 };
    const Vec3d axes[3] = { a0, b1 * std::cos(ph) + b2 * std::sin(ph), b1 * -std::sin(ph) + b2 * std::cos(ph) };
    const Vec3d center = { 5, -3, 2 };
    ConvexElem box;
    Check(box.HullFromPlanes(BoxPlanes(center, axes, { 1.0, 0.5, 0.7 }), {}), "rotated hull");

    // 중심 근처를 지나는 정규화된 기울어진 평면으로 자른다.
    const double len = std::sqrt(1.0 + 4.0 + 0.25);
    const Vec3d n = { 1.0 / len, 2.0 / len, -0.5 / len };
    const Plane plane = { n, Dot(n, center) + 0.1 };
    AggregateGeom keep;
    AggregateGeom other;
    SliceAggregateGeom(Wrap(box), plane, keep, other);

    // 양쪽이 모두 있고 각 정점이 자기 쪽에 있으며 부피 합이 부모와 같다.
    Check(!keep.ConvexElems.empty() && !other.ConvexElems.empty(), "rotated pieces");
    Check(AllOnSide(keep, plane, 1.0, 1e-9) && AllOnSide(other, plane, -1.0, 1e-9), "rotated sides");
    const double parentVolume = box.GetVolume();
    CheckNear(TotalVolume(keep) + TotalVolume(other), parentVolume, parentVolume * 1e-9, "rotated sum");
}

// 정점 병합(0.001 m)보다 얇은 조각과 두꺼운 대조 조각의 절단 결과를 검사한다.
static void TestThinSlice()
{
    // 두께 0.0005 m 조각을 만드는 평면의 결과를 확인한다.
    const AggregateGeom cube = Wrap(MakeBox({ 0, 0, 0 }, { 1, 1, 1 }));
    const Plane thin = { { 0, 1, 0 }, 0.9995 };
    AggregateGeom keep;
    AggregateGeom other;
    SliceAggregateGeom(cube, thin, keep, other);
    // 병합(0.001 m)보다 얇은 앞쪽 조각은 살아남지만 정점 병합으로 일그러진다(부피 약 1/6로 감소).
    Check(keep.ConvexElems.size() == 1 && other.ConvexElems.size() == 1, "thin pieces");
    Check(TotalVolume(keep) > 0.0 && TotalVolume(keep) <= 0.002 + 1e-9, "thin keep distorted");
    Check(AllOnSide(keep, thin, 1.0, MinHullVertDistance), "thin keep side");

    // 뒤쪽은 방향과 부피가 정확하다(8 - 2·2·0.0005).
    Check(AllOnSide(other, thin, -1.0, 1e-9), "thin other side");
    CheckNear(TotalVolume(other), 8.0 - 0.002, 1e-6, "thin other volume");

    // 두께 0.01 m 대조 조각은 양쪽이 살아남고 부피 합이 부모와 같다.
    const Plane thick = { { 0, 1, 0 }, 0.99 };
    SliceAggregateGeom(cube, thick, keep, other);
    Check(keep.ConvexElems.size() == 1 && other.ConvexElems.size() == 1, "thick pieces");
    CheckNear(TotalVolume(keep) + TotalVolume(other), 8.0, 8e-6, "thick sum");
    Check(AllOnSide(keep, thick, 1.0, 1e-9) && AllOnSide(other, thick, -1.0, 1e-9), "thick sides");
}

// 지지 평면이 4개 미만인 입력은 실패하고 채워진 out이 완전히 비워지는지 검사한다.
static void TestSliceFailureResets()
{
    // 빈 요소를 입력으로, 미리 채운 out을 결과로 넘긴다.
    ConvexElem filled = MakeBox({ 0, 0, 0 }, { 1, 1, 1 });
    const ConvexElem empty;
    Check(!SliceConvexElem(empty, { { 0, 1, 0 }, 0.0 }, filled), "empty input fails");

    // 정점·인덱스·경계 상자가 모두 비워진다.
    Check(filled.VertexData.empty() && filled.IndexData.empty() && !filled.ElemBox.IsValid, "out reset");
}

// 모든 테스트를 실행하고 검사 횟수를 출력한다.
int main()
{
    // 모든 테스트를 순서대로 실행하고 예외가 나면 실패로 끝낸다.
    try
    {
        TestBoxPlaneCompare();
        TestCubeSlice();
        TestCubeDiagonalHullContainment();
        TestPassThroughAndFace();
        TestCornerTetra();
        TestTwoElems();
        TestRepeatedSlice();
        TestRotatedOffCenter();
        TestThinSlice();
        TestSliceFailureResets();
    }
    catch (const std::exception& e)
    {
        // 실패 메시지와 지금까지의 검사 횟수를 출력한다.
        std::printf("FAILED: %s (after %zu checks)\n", e.what(), g_Checks);
        return 1;
    }

    // 통과한 검사 횟수를 출력한다.
    std::printf("AggregateSliceTests passed: %zu checks\n", g_Checks);
    return 0;
}
