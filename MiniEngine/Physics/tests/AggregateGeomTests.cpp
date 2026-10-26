#include "CollisionTestUtils.h"
#include "Collision/AggregateGeom.h"
#include <algorithm>
#include <cstdio>
#include <random>

// 구현과 독립으로 18개 단위 방향(축 6개, 대각 12개)을 만든다.
static std::vector<Vec3d> TestDirs()
{
    // 축 방향 6개를 넣는다.
    const double r = std::sqrt(0.5);
    std::vector<Vec3d> dirs = { {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}, {0,0,1}, {0,0,-1} };

    // 두 축 조합마다 부호가 있는 대각 방향 4개를 넣는다.
    const int axes[3][2] = { {0, 1}, {0, 2}, {1, 2} };
    for (const auto& ab : axes)
    {
        for (int sa = -1; sa <= 1; sa += 2)
        {
            for (int sb = -1; sb <= 1; sb += 2)
            {
                double c[3] = { 0, 0, 0 };
                c[ab[0]] = sa * r;
                c[ab[1]] = sb * r;
                dirs.push_back({ c[0], c[1], c[2] });
            }
        }
    }

    // 만들어진 방향 목록을 돌려준다.
    return dirs;
}

// 점 집합에서 방향별 최댓값과 팽창량으로 18평면을 직접 만든다.
static std::vector<Plane> TestPlanes(const std::vector<Vec3d>& pts, double minSize)
{
    // 방향마다 최대 투영값에 팽창량을 더해 평면을 추가한다.
    std::vector<Plane> planes;
    for (const Vec3d& d : TestDirs())
    {
        double m = Dot(d, pts[0]);
        for (const Vec3d& p : pts)
        {
            m = std::max(m, Dot(d, p));
        }
        planes.push_back({ d, m + minSize });
    }

    // 만들어진 평면 목록을 돌려준다.
    return planes;
}

// hull의 모든 지지 평면 안쪽에 점들이 들어 있는지 검사한다.
static bool Contains(const ConvexElem& e, const std::vector<Vec3d>& pts)
{
    // 점마다 모든 평면에서 허용 오차 이내인지 확인한다.
    std::vector<Plane> planes;
    e.GetPlanes(planes);
    for (const Vec3d& p : pts)
    {
        for (const Plane& pl : planes)
        {
            if (pl.PlaneDot(p) > LocalEpsDistance)
            {
                return false;
            }
        }
    }

    // 모든 점이 안쪽이라고 알린다.
    return true;
}

// 네 점이 이루는 사면체의 부피를 돌려준다.
static double TetraVolume(Vec3d a, Vec3d b, Vec3d c, Vec3d d)
{
    // 세 모서리의 스칼라 삼중곱을 6으로 나눈다.
    return std::fabs(Dot(Cross(b - a, c - a), d - a)) / 6.0;
}

// 두 점 집합이 허용 오차 안에서 서로의 모든 점과 대응하는지(양방향) 검사한다.
static bool SameSet(const std::vector<Vec3d>& a, const std::vector<Vec3d>& b, double tol)
{
    // 한쪽 집합의 모든 점에 다른 쪽의 가까운 점이 있는지 확인하는 람다를 만든다.
    auto covered = [tol](const std::vector<Vec3d>& from, const std::vector<Vec3d>& to)
    {
        // from의 점마다 to에서 가까운 점을 찾는다.
        for (const Vec3d& p : from)
        {
            bool found = false;
            for (const Vec3d& q : to)
            {
                found = found || Dot(p - q, p - q) <= tol * tol;
            }
            if (!found)
            {
                return false;
            }
        }

        // 모든 점이 대응하면 참이다.
        return true;
    };

    // 양쪽 방향 모두 대응해야 한다.
    return covered(a, b) && covered(b, a);
}

// 평면 세 개의 교점 중 모든 평면 안쪽에 있는 점을 구현과 독립적으로 모두 구한다.
static std::vector<Vec3d> EnumerateVertices(const std::vector<Plane>& planes)
{
    // 모든 세 평면 조합을 Cramer 공식으로 풀고 거의 특이한 조합은 건너뛴다.
    std::vector<Vec3d> out;
    for (size_t i = 0; i < planes.size(); ++i)
    {
        for (size_t j = i + 1; j < planes.size(); ++j)
        {
            for (size_t k = j + 1; k < planes.size(); ++k)
            {
                const Plane& a = planes[i];
                const Plane& b = planes[j];
                const Plane& c = planes[k];
                const double det = Dot(a.Normal, Cross(b.Normal, c.Normal));
                if (std::fabs(det) < 1e-6)
                {
                    continue;
                }
                const Vec3d x = (Cross(b.Normal, c.Normal) * a.W + Cross(c.Normal, a.Normal) * b.W + Cross(a.Normal, b.Normal) * c.W) * (1.0 / det);

                // 모든 평면 안쪽이고 중복이 아니면 추가한다.
                bool inside = true;
                for (const Plane& p : planes)
                {
                    inside = inside && p.PlaneDot(x) <= 1e-7;
                }
                bool dup = false;
                for (const Vec3d& v : out)
                {
                    dup = dup || Dot(x - v, x - v) < 1e-12;
                }
                if (inside && !dup)
                {
                    out.push_back(x);
                }
            }
        }
    }

    // 구한 꼭짓점 목록을 돌려준다.
    return out;
}

// 점 집합의 18-DOP이 반공간 교집합과 같고 최종 hull이 포함·부피 허용오차를 만족하는지 검사한다. volTol은 병합 전 부피 대비 허용 손실 비율이다.
static void CheckKDop(const std::vector<Vec3d>& pts, const char* name, double volTol)
{
    // 동작: 독립 열거와 생산 코드의 병합 전 후보를 만든다.
    const std::vector<Plane> planes = TestPlanes(pts, KDopMinSize);
    const std::vector<Vec3d> exact = EnumerateVertices(planes);
    std::vector<Vec3d> candidates;
    CollectPlanePolygonVertices(planes, HullTolerances(), candidates);

    // 검사: 두 꼭짓점 집합이 양방향으로 일치한다.
    Check(!exact.empty() && SameSet(exact, candidates, 1e-6), name);

    // 동작: 병합 거리를 거의 없앤 정확한 병합 전 부피와 최종 hull을 만든다.
    HullTolerances tiny;
    tiny.MinVertDistance = 1e-9;
    ConvexElem pre;
    Check(pre.HullFromPlanes(planes, {}, DefaultSnapDistSq, tiny), "Premerge hull builds");
    AggregateGeom g;
    Check(GenerateKDop18(pts, g), "KDOP builds");

    // 검사: 최종 hull은 입력을 포함하고, R2 허용오차 V_pre >= V_final >= V_pre x (1 - 1e-2)를 만족한다.
    const double vPre = pre.GetVolume();
    const double vFinal = g.ConvexElems[0].GetVolume();
    Check(IsClosedSurface(g.ConvexElems[0]) && Contains(g.ConvexElems[0], pts), "Final hull closed and contains");
    Check(vFinal <= vPre + 1e-9 && vFinal >= vPre * (1.0 - volTol), "Final volume within R2 tolerance");
}

// 단위 큐브 점으로 성공, 닫힘, 포함, 부피 범위를 검사한다.
static void TestCube()
{
    // 동작: (±1)^3 여덟 점으로 18-DOP을 만든다.
    std::vector<Vec3d> pts;
    for (int i = 0; i < 8; ++i)
    {
        pts.push_back({ (i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0 });
    }
    AggregateGeom g;
    Check(GenerateKDop18(pts, g) && g.ConvexElems.size() == 1, "Cube KDOP builds one elem");

    // 검사: 닫힌 곡면이고 입력 점이 모두 안쪽이다.
    const ConvexElem& e = g.ConvexElems[0];
    Check(IsClosedSurface(e), "Cube KDOP closed");
    Check(Contains(e, pts), "Cube KDOP contains inputs");

    // 검사: 부피가 큐브 이상이고 팽창한 큐브 이하이며 정점이 8개 이상이다.
    const double vol = e.GetVolume();
    const double side = 2.0 + 2.0 * KDopMinSize;
    Check(vol >= 8.0 && vol <= side * side * side + 1e-9, "Cube KDOP volume range");
    Check(e.VertexData.size() >= 8, "Cube KDOP vertex count");

    // 검사: 반공간 동일성과 부피 허용오차.
    CheckKDop(pts, "Cube premerge vertices match half-space enumeration", 1e-2);
}

// 얇은 상자 점으로도 성공하고 반공간 동일성을 만족하는지 검사한다.
static void TestThinBox()
{
    // 동작: 2 x 0.01 x 2 상자 여덟 점으로 만든다.
    std::vector<Vec3d> pts;
    for (int i = 0; i < 8; ++i)
    {
        pts.push_back({ (i & 1) ? 1.0 : -1.0, (i & 2) ? 0.005 : -0.005, (i & 4) ? 1.0 : -1.0 });
    }

    // 검사: 두께 0.012 m 상자는 정점 병합 0.001 m로 부피가 약 10% 줄어(Unreal 0.1 cm 병합과 같은 성질) 허용오차를 0.15로 둔다.
    CheckKDop(pts, "Thin box premerge vertices match half-space enumeration", 0.15);
}

// 원점 밖 회전 타원체 점 구름의 동일성, 포함, 부피 하한을 검사한다.
static void TestRotatedCloud()
{
    // 준비: 회전축 (1,2,3)과 평행이동으로 고정 시드 점 200개를 만든다.
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> uni(-1.0, 1.0);
    const double len = std::sqrt(14.0);
    const Vec3d k{ 1 / len, 2 / len, 3 / len };
    const double ang = 0.9;
    const Vec3d offset{ 10, -5, 7 };
    std::vector<Vec3d> pts;
    while (pts.size() < 200)
    {
        // 단위 구 안의 점을 뽑아 타원체로 늘린다.
        const Vec3d u{ uni(rng), uni(rng), uni(rng) };
        if (Dot(u, u) > 1.0)
        {
            continue;
        }
        const Vec3d s{ u.x * 3.0, u.y * 1.0, u.z * 0.5 };

        // Rodrigues 공식으로 회전하고 평행이동한다.
        const Vec3d r = s * std::cos(ang) + Cross(k, s) * std::sin(ang) + k * (Dot(k, s) * (1 - std::cos(ang)));
        pts.push_back(r + offset);
    }

    // 검사: 반공간 동일성, 포함, 부피 허용오차.
    CheckKDop(pts, "Cloud premerge vertices match half-space enumeration", 1e-2);

    // 동작: 18-DOP을 만든다.
    AggregateGeom g;
    Check(GenerateKDop18(pts, g), "Cloud KDOP builds");

    // 준비: 축별 극단 점 6개를 모은다.
    std::vector<Vec3d> ext;
    for (int axis = 0; axis < 3; ++axis)
    {
        // 축 성분 하나로 두 점의 크기를 비교한다.
        auto comp = [axis](const Vec3d& a, const Vec3d& b)
        {
            // 두 점의 축 성분을 배열로 꺼낸다.
            const double av[3] = { a.x, a.y, a.z };
            const double bv[3] = { b.x, b.y, b.z };

            // 선택한 축 성분이 작은 쪽을 앞세운다.
            return av[axis] < bv[axis];
        };
        ext.push_back(*std::min_element(pts.begin(), pts.end(), comp));
        ext.push_back(*std::max_element(pts.begin(), pts.end(), comp));
    }

    // 극단 점 네 개 조합 중 최대 사면체 부피를 구한다.
    double best = 0.0;
    for (int a = 0; a < 6; ++a)
    {
        for (int b = a + 1; b < 6; ++b)
        {
            for (int c = b + 1; c < 6; ++c)
            {
                for (int d = c + 1; d < 6; ++d)
                {
                    best = std::max(best, TetraVolume(ext[a], ext[b], ext[c], ext[d]));
                }
            }
        }
    }

    // 검사: 18-DOP 부피가 입력 볼록 껍질의 하한 이상이다.
    Check(best > 0.1 && g.ConvexElems[0].GetVolume() >= best, "Cloud volume above tetra lower bound");
}

// 빈 입력과 점 하나 입력의 실제 동작을 검사한다.
static void TestDegenerateInputs()
{
    // 검사: 빈 입력은 실패하고 결과가 비어 있다.
    AggregateGeom g;
    g.ConvexElems.push_back(ConvexElem());
    Check(!GenerateKDop18({}, g) && g.ConvexElems.empty(), "Empty input fails and clears");

    // 검사: 점 하나는 성공하고 그 점을 포함한다. x, y는 약 0.002 m이고 z는 정점 병합으로 줄어든다(실제 동작).
    const std::vector<Vec3d> one = { {5, -3, 2} };
    Check(GenerateKDop18(one, g) && g.ConvexElems.size() == 1, "Single point builds");
    const ConvexElem& e = g.ConvexElems[0];
    Check(Contains(e, one), "Single point result contains the point");
    const Vec3d ext = e.ElemBox.Max - e.ElemBox.Min;
    CheckNear(ext.x, 2.0 * KDopMinSize, 1e-6, "Single point x extent");
    CheckNear(ext.y, 2.0 * KDopMinSize, 1e-6, "Single point y extent");
    Check(ext.z > 0.0005 && ext.z <= 2.0 * KDopMinSize + 1e-6, "Single point z extent shrinks by merge but stays positive");
}

// snap 거리제곱이 Unreal cm 공식(0.0015 x R_cm cm²)을 m²로 환산한 값과 같은지 검사한다.
static void TestSnapFormula()
{
    // 여러 반지름에서 cm 공식 결과에 1e-4를 곱한 값과 비교한다.
    const double radii[] = { 0.01, 0.5, 1.0, 2.5, 100.0 };
    for (double r : radii)
    {
        const double expected = 0.0015 * (r * 100.0) * 1e-4;
        CheckNear(KDopSnapDistSq(r) / expected, 1.0, 1e-12, "Snap dist formula matches Unreal cm");
    }
}

// cm 단위 Unreal 상수 경로와 m 단위 경로가 같은 위상·좌표·부피 비율을 만드는지 검사한다.
static void TestUnitConsistency()
{
    // 준비: 큐브 점의 m 단위와 x100 cm 단위 집합을 만든다.
    std::vector<Vec3d> ptsM;
    std::vector<Vec3d> ptsCm;
    for (int i = 0; i < 8; ++i)
    {
        const Vec3d p{ (i & 1) ? 1.0 : -1.0, (i & 2) ? 1.0 : -1.0, (i & 4) ? 1.0 : -1.0 };
        ptsM.push_back(p);
        ptsCm.push_back(p * 100.0);
    }

    // 준비: Unreal cm 허용 오차와 MinSize 0.1로 병합 전 후보를 구한다.
    HullTolerances cm;
    cm.MinVertDistance = 0.1;
    cm.MinArea = 0.001;
    cm.BigHalfExtent = 1048576.0;
    cm.EpsAngle = 1e-8;
    cm.EpsDistance = 1e-8;
    const std::vector<Plane> planes = TestPlanes(ptsCm, 0.1);
    std::vector<Vec3d> cand;
    CollectPlanePolygonVertices(planes, cm, cand);

    // 후보의 AABB 중심에서 가장 먼 정점까지의 거리로 반지름을 구한다.
    Vec3d lo = cand[0];
    Vec3d hi = cand[0];
    for (const Vec3d& v : cand)
    {
        lo = { std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z) };
        hi = { std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z) };
    }
    const Vec3d center = (lo + hi) * 0.5;
    double radiusSq = 0.0;
    for (const Vec3d& v : cand)
    {
        radiusSq = std::max(radiusSq, Dot(v - center, v - center));
    }

    // 동작: Unreal 0.0015 x R 거리로 snap한 cm hull과 m 단위 GenerateKDop18 결과를 만든다.
    ConvexElem inCm;
    Check(inCm.HullFromPlanes(planes, cand, 0.0015 * std::sqrt(radiusSq), cm), "Cm snapped hull builds");
    AggregateGeom g;
    Check(GenerateKDop18(ptsM, g), "Meter KDOP builds");
    const ConvexElem& inM = g.ConvexElems[0];

    // 검사: 정점·삼각형 수가 같고 부피 비율이 1e6이다.
    Check(inCm.VertexData.size() == inM.VertexData.size(), "Same vertex count across units");
    Check(inCm.IndexData.size() == inM.IndexData.size(), "Same triangle count across units");
    CheckNear(inCm.GetVolume() / inM.GetVolume() / 1e6, 1.0, 1e-6, "Volume ratio is 1e6");

    // 검사: m 정점 x100이 cm 정점과 양방향으로 대응한다.
    std::vector<Vec3d> scaled;
    for (const Vec3d& v : inM.VertexData)
    {
        scaled.push_back(v * 100.0);
    }
    Check(SameSet(scaled, inCm.VertexData, 1e-6), "Vertex coordinates match across units");
}

// 모든 검사를 실행하고 실패 시 비정상 종료 코드를 돌려준다.
int main()
{
    // 모든 테스트를 순서대로 실행하고 예외가 나면 실패로 끝낸다.
    try
    {
        TestCube();
        TestThinBox();
        TestRotatedCloud();
        TestDegenerateInputs();
        TestSnapFormula();
        TestUnitConsistency();
    }
    catch (const std::exception& e)
    {
        // 실패 메시지와 지금까지의 검사 횟수를 출력한다.
        std::printf("FAILED: %s (after %zu checks)\n", e.what(), g_Checks);
        return 1;
    }

    // 통과한 검사 횟수를 출력한다.
    std::printf("AggregateGeomTests passed: %zu checks\n", g_Checks);
    return 0;
}
