#include "CollisionTestUtils.h"
#include <cmath>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <utility>

// 모든 삼각형이 hull 중심에서 바깥을 향하는지 검사한다.
static bool OutwardWinding(const ConvexElem& e)
{
    // 정점 평균으로 hull 중심을 구한다.
    Vec3d center{ 0, 0, 0 };
    for (const Vec3d& v : e.VertexData)
    {
        center = center + v * (1.0 / static_cast<double>(e.VertexData.size()));
    }

    // 삼각형 법선이 중심에서 바깥을 향하는지 확인한다.
    for (size_t t = 0; t + 2 < e.IndexData.size(); t += 3)
    {
        const Vec3d a = e.VertexData[e.IndexData[t]];
        const Vec3d n = Cross(e.VertexData[e.IndexData[t + 1]] - a, e.VertexData[e.IndexData[t + 2]] - a);
        if (Dot(n, a - center) <= 0.0)
        {
            return false;
        }
    }

    // 모든 삼각형이 바깥을 향한다고 알린다.
    return true;
}

// 넓이가 0에 가까운 퇴화 삼각형이 있는지 검사한다.
static bool HasDegenerateTriangle(const ConvexElem& e)
{
    // 삼각형 외적 길이 제곱이 임계보다 작으면 퇴화로 본다.
    for (size_t t = 0; t + 2 < e.IndexData.size(); t += 3)
    {
        const Vec3d a = e.VertexData[e.IndexData[t]];
        const Vec3d n = Cross(e.VertexData[e.IndexData[t + 1]] - a, e.VertexData[e.IndexData[t + 2]] - a);
        if (Dot(n, n) < 1e-20)
        {
            return true;
        }
    }

    // 퇴화 삼각형이 없다고 알린다.
    return false;
}

// 모든 정점이 모든 평면 안쪽에 있는지 검사한다(정의는 아래).
static bool AllVerticesInside(const std::vector<Plane>& planes, const std::vector<Vec3d>& verts, double tol);

// 정육면체의 정점 수·부피·감김·경계 상자·닫힌 곡면·평면 왕복을 검사한다.
static void TestCube()
{
    // 준비: 한 변 2인 정육면체 hull을 만든다.
    ConvexElem cube;
    Check(cube.HullFromPlanes(AlignedBox({0,0,0}, {1,1,1}), {}), "Cube builds");

    // 검사: 정점·삼각형 수, 부피, 감김, 닫힌 곡면, 경계 상자를 확인한다.
    Check(cube.VertexData.size() == 8 && cube.IndexData.size() == 36, "Cube 8 verts 12 triangles");
    CheckNear(cube.GetVolume(), 8.0, 1e-9, "Cube volume");
    Check(OutwardWinding(cube), "Outward winding");
    Check(IsClosedSurface(cube) && !HasDegenerateTriangle(cube), "Closed, no degenerate triangles");
    Check(cube.ElemBox.IsValid && std::fabs(cube.ElemBox.Min.x + 1) < 1e-9 && std::fabs(cube.ElemBox.Max.z - 1) < 1e-9, "Elem box");

    // 동작: hull에서 지지 평면을 꺼낸다.
    std::vector<Plane> planes;
    cube.GetPlanes(planes);

    // 검사: 평면 6개이고 모든 정점이 그 안쪽에 있다.
    Check(planes.size() == 6 && AllVerticesInside(planes, cube.VertexData, LocalEpsDistance), "Cube yields 6 supporting planes");

    // 동작: 꺼낸 평면으로 hull을 다시 만든다.
    ConvexElem again;
    Check(again.HullFromPlanes(planes, cube.VertexData), "Round trip builds");

    // 검사: 왕복 후 부피가 같다.
    CheckNear(again.GetVolume(), 8.0, 1e-9, "Round trip volume");
}

// 같은 평면을 두 번 넣어도 면이 중복되지 않는다.
static void TestDuplicatePlane()
{
    // 준비: 상자 평면에 첫 평면을 한 번 더 넣는다.
    std::vector<Plane> planes = AlignedBox({0,0,0}, {1,1,1});
    planes.push_back(planes[0]);

    // 동작: hull을 만든다.
    ConvexElem e;
    Check(e.HullFromPlanes(planes, {}), "Duplicate plane builds");

    // 검사: 부피가 같고 삼각형 12개가 깨끗하게 남는다.
    CheckNear(e.GetVolume(), 8.0, 1e-9, "Duplicate plane volume");
    Check(e.IndexData.size() == 36 && !HasDegenerateTriangle(e) && IsClosedSurface(e), "Duplicate plane 12 clean triangles");
}

// 거의 평행한 얕은 면이 병합되거나 삭제되지 않고 왕복 후에도 부피가 같다.
static void TestShallowFace()
{
    // 준비: 긴 상자에 거의 평행한 얕은 평면을 하나 더한다.
    std::vector<Plane> planes = AlignedBox({0,0,0}, {1,100,1});
    const double len = std::sqrt(1.0 + 1e-8);
    planes.push_back({ {1.0 / len, 1e-4 / len, 0}, 0.99995 / len });

    // 동작: hull을 만들고 지지 평면을 꺼낸다.
    ConvexElem e;
    Check(e.HullFromPlanes(planes, {}), "Shallow hull builds");
    std::vector<Plane> back;
    e.GetPlanes(back);

    // 검사: 얕은 면이 일곱 번째 평면으로 남는다.
    Check(back.size() == 7, "Shallow face survives as 7th plane");

    // 동작: 꺼낸 평면으로 hull을 다시 만든다.
    ConvexElem again;
    Check(again.HullFromPlanes(back, e.VertexData), "Shallow round trip builds");

    // 검사: 왕복 후 부피가 같고 원본은 닫힌 곡면이다.
    CheckNear(again.GetVolume(), e.GetVolume(), 1e-9, "Shallow round trip volume");
    Check(IsClosedSurface(e), "Shallow hull closed");
}

// snap은 임계 안의 정점만 목표로 옮기고 밖이면 유지한다. 가까운 정점은 병합한다.
static void TestSnapAndMerge()
{
    // 준비: 상자 평면과 임계 안·밖의 snap 목표를 만든다.
    const std::vector<Plane> box = AlignedBox({0,0,0}, {1,1,1});
    const Vec3d near{ 1, 1, 1.0000005 };
    const Vec3d far{ 1, 1, 1.000002 };

    // 동작: 임계 안의 목표로 snap해 hull을 만든다.
    ConvexElem e;
    Check(e.HullFromPlanes(box, { near }), "Snap builds");
    bool hasNear = false;
    for (const Vec3d& v : e.VertexData)
    {
        hasNear |= (v.x == near.x && v.y == near.y && v.z == near.z);
    }

    // 검사: 정점이 목표로 옮겨졌다.
    Check(hasNear && IsClosedSurface(e), "Vertex snapped within threshold");

    // 동작: 임계 밖의 목표로 snap해 hull을 만든다.
    Check(e.HullFromPlanes(box, { far }), "Far snap builds");
    bool hasFar = false;
    for (const Vec3d& v : e.VertexData)
    {
        hasFar |= (v.z == far.z);
    }

    // 검사: 정점이 옮겨지지 않았다.
    Check(!hasFar, "Not snapped beyond threshold");

    // 모서리를 자르는 평면: 다리 길이 0.0004면 세 정점이 병합되고 0.01이면 10정점이다.
    const double inv = 1.0 / std::sqrt(3.0);
    std::vector<Plane> cut = box;
    cut.push_back({ {inv, inv, inv}, (3.0 - 0.0004) * inv });

    // 검사: 가까운 정점은 병합되어 8개다.
    Check(e.HullFromPlanes(cut, {}) && e.VertexData.size() == 8 && IsClosedSurface(e), "Close vertices merge");

    // 동작: 절단 평면을 0.01 깊이로 옮긴다.
    cut.back().W = (3.0 - 0.01) * inv;

    // 검사: 먼 정점은 병합되지 않아 10개다.
    Check(e.HullFromPlanes(cut, {}) && e.VertexData.size() == 10 && IsClosedSurface(e), "Distant vertices stay");
}

// 퇴화 입력과 각 실패 분기를 허용 오차 조정으로 독립 검사한다.
static void TestDegenerate()
{
    // 검사 대상 요소를 준비한다.
    ConvexElem e;

    // 검사: 평면이 없거나 모순이거나 두께가 0인 입력은 실패한다.
    Check(!e.HullFromPlanes({}, {}), "No planes");
    Check(!e.HullFromPlanes({ { {0,0,1}, 0 } }, {}), "Single plane is flat");
    Check(!e.HullFromPlanes({ { {0,0,1}, -1 }, { {0,0,-1}, -1 } }, {}), "Contradictory planes");
    Check(!e.HullFromPlanes(AlignedBox({0,0,0}, {1,1,0}), {}), "Zero thickness slab");
    Check(!e.HullFromPlanes(AlignedBox({0,0,0}, {0,0,1}), {}), "Zero width needle");
    Check(!e.ElemBox.IsValid, "Failed hull has no box");

    // 준비: 정상 상자와 기본 허용 오차를 만든다.
    const std::vector<Plane> cube = AlignedBox({0,0,0}, {1,1,1});
    HullTolerances tol;

    // 검사: 최소 면적을 키우면 면적 분기에서 실패한다.
    tol.MinArea = 1e3;
    Check(!e.HullFromPlanes(cube, {}, DefaultSnapDistSq, tol), "Min area branch");

    // 검사: 각도 허용치를 키우면 공선 분기에서 실패한다.
    tol = HullTolerances();
    tol.EpsAngle = 3.0;
    Check(!e.HullFromPlanes(cube, {}, DefaultSnapDistSq, tol), "Colinear branch");

    // 두께 2e-11 m 슬랩: 위아래 정점이 병합되고 남은 정점이 공면 허용치(1e-10 m) 안에 있어 공면 실패한다.
    Check(!e.HullFromPlanes(AlignedBox({0,0,0}, {1,1,1e-11}), {}), "Coplanar branch");
}

// 클리퍼가 평면 근처 정점에서 외삽하지 않는 회귀 검사를 한다.
static void TestClipperBoundary()
{
    // 준비: 평면 위 허용치 안팎에 걸친 삼각형을 만든다.
    const double eps = 1e-10;
    const Plane plane{ {0,0,1}, 0.0 };
    const std::vector<Vec3d> poly = { {0, 0, 0.5 * eps}, {1, 0, 2 * eps}, {0, 1, -5e-3} };

    // 동작: 평면 안쪽으로 자른다.
    const std::vector<Vec3d> out = ClipPolygonInside(poly, plane, eps);

    // 검사: 다각형이 유지되고 모든 정점이 원래 범위 안에 있다.
    Check(out.size() >= 3, "Clip keeps polygon");
    for (const Vec3d& p : out)
    {
        Check(p.x >= 0.0 && p.x <= 1.0 && p.y >= 0.0 && p.y <= 1.0, "No extrapolated vertex");
    }
}

// 원점 밖에서 회전한 상자와 먼 좌표의 작은 상자 부피를 검사한다.
static void TestRotatedAndFarBox()
{
    // 준비: 회전 상자의 세 축을 만든다.
    const double c1 = std::cos(0.5);
    const double s1 = std::sin(0.5);
    const double c2 = std::cos(0.3);
    const double s2 = std::sin(0.3);
    const Vec3d axes[3] = { {c1, s1, 0}, {-s1*c2, c1*c2, s2}, {s1*s2, -c1*s2, c2} };

    // 동작: 회전 상자 hull을 만든다.
    ConvexElem e;
    Check(e.HullFromPlanes(BoxPlanes({5,-7,3}, axes, {1,2,3}), {}), "Rotated box builds");

    // 검사: 정점 8개와 부피 48을 확인한다.
    Check(e.VertexData.size() == 8, "Rotated box 8 verts");
    CheckNear(e.GetVolume(), 48.0, 1e-9, "Rotated box volume");

    // 동작: 먼 좌표의 작은 상자 hull을 만든다.
    ConvexElem far;
    Check(far.HullFromPlanes(AlignedBox({1000,1000,1000}, {0.01,0.01,0.01}), {}), "Far small box builds");

    // 검사: 큰 좌표에서도 부피가 유지된다.
    CheckNear(far.GetVolume(), 8e-6, 1e-12, "Far small box volume");
}

// 같은 상자를 cm와 Unreal 임계, m와 환산 임계로 만들어 정점·snap·부피가 일치하는지 검사한다.
static void TestUnitConsistency()
{
    // 준비: m 단위 snap 목표와 ×100한 cm 단위 목표를 만든다.
    const double inv = 1.0 / std::sqrt(3.0);
    const Vec3d nearM{ -1, -1, -1 - 5e-7 };
    const Vec3d farM{ -1, 1, 1 + 2e-6 };
    const std::vector<Vec3d> snapM = { nearM, farM };
    std::vector<Vec3d> snapCm;
    for (const Vec3d& s : snapM)
    {
        snapCm.push_back(s * 100.0);
    }

    // 주어진 단위 배율·임계로 모서리가 잘린 상자를 만든다.
    auto build = [&](double scale, const std::vector<Vec3d>& snap, const HullTolerances& tol, double snapDistSq)
    {
        std::vector<Plane> planes = AlignedBox({0,0,0}, { scale, scale, scale });
        planes.push_back({ {inv, inv, inv}, (3.0 - 0.0004) * scale * inv });
        ConvexElem e;
        Check(e.HullFromPlanes(planes, snap, snapDistSq, tol), "Unit hull builds");
        return e;
    };

    // 준비: Unreal cm 단위 허용 오차를 채운다.
    HullTolerances cm;
    cm.MinVertDistance = 0.1;
    cm.MinArea = 0.001;
    cm.BigHalfExtent = 1048576.0;
    cm.EpsAngle = 1e-8;
    cm.EpsDistance = 1e-8;

    // 동작: cm 단위와 m 단위로 같은 상자를 만든다.
    const ConvexElem inCm = build(100.0, snapCm, cm, 1e-8);
    const ConvexElem inM = build(1.0, snapM, HullTolerances(), DefaultSnapDistSq);

    // 검사: 두 단위의 정점·삼각형 수가 같다.
    Check(inCm.VertexData.size() == inM.VertexData.size(), "Same vertex count across units");
    Check(inCm.IndexData.size() == inM.IndexData.size(), "Same triangle count across units");

    // m 정점마다 ×100한 cm 정점이 대응해야 한다. snap 안쪽 목표는 정확히 쓰이고 바깥 목표는 쓰이지 않는다.
    bool usedNear = false;
    bool usedFar = false;
    for (const Vec3d& m : inM.VertexData)
    {
        bool matched = false;
        for (const Vec3d& c : inCm.VertexData)
        {
            const Vec3d d = c - m * 100.0;
            matched |= Dot(d, d) < 1e-12;
        }
        Check(matched, "Vertex corresponds across units");
        usedNear |= (m.x == nearM.x && m.y == nearM.y && m.z == nearM.z);
        usedFar |= (m.z == farM.z);
    }

    // 검사: snap 사용 여부와 부피 비율을 확인한다.
    Check(usedNear && !usedFar, "Snap inside used, outside ignored");
    CheckNear(inCm.GetVolume() / inM.GetVolume(), 1e6, 1e-2, "Volume ratio 1e6");
}

// 모든 평면에 대해 모든 정점이 PlaneDot <= tol인지 검사한다.
static bool AllVerticesInside(const std::vector<Plane>& planes, const std::vector<Vec3d>& verts, double tol)
{
    // 평면마다 모든 정점의 부호 있는 거리를 확인한다.
    for (const Plane& plane : planes)
    {
        for (const Vec3d& v : verts)
        {
            if (plane.PlaneDot(v) > tol)
            {
                return false;
            }
        }
    }

    // 모든 정점이 모든 평면 안쪽이라고 알린다.
    return true;
}

// 큐브에 면 중심점 또는 변 중점을 더해도 모서리 8개·삼각형 12개로 정리되는지 검사한다.
static void TestInteriorAndEdgePoints()
{
    // 준비: 면 위 점과 변 위 점을 추가 후보로 둔다.
    const Vec3d extras[2] = { {1, 0, 0}, {1, 1, 0} };

    // 추가 점마다 큐브에 넣어 정리 결과를 검사한다.
    for (const Vec3d& extra : extras)
    {
        // 동작: 큐브에 추가 점을 넣고 인덱스를 다시 계산한다.
        ConvexElem e;
        Check(e.HullFromPlanes(AlignedBox({0,0,0}, {1,1,1}), {}), "Cube for extra point");
        e.VertexData.push_back(extra);
        Check(e.ComputeHullIndices(), "Rebuild with extra point");

        // 검사: 추가 점이 제거되고 hull이 깨끗하며 부피가 같다.
        Check(e.VertexData.size() == 8 && e.IndexData.size() == 36, "Extra point removed, 8 verts 12 triangles");
        Check(IsClosedSurface(e) && !HasDegenerateTriangle(e) && OutwardWinding(e), "Extra point hull clean");
        CheckNear(e.GetVolume(), 8.0, 1e-9, "Extra point volume");
    }
}

// 한 정점을 1e-6 m 들어 올린 비평면 사각 면도 닫힌 곡면, 바깥 감김, 올바른 부피를 유지한다.
static void TestNonPlanarQuad()
{
    // 준비: 큐브를 만든다.
    ConvexElem e;
    Check(e.HullFromPlanes(AlignedBox({0,0,0}, {1,1,1}), {}), "Cube for lifted corner");

    // 동작: 한 모서리 정점을 1e-6 들어 올리고 인덱스를 다시 계산한다.
    for (Vec3d& v : e.VertexData)
    {
        if (v.x > 0 && v.y > 0 && v.z > 0)
        {
            v.z += 1e-6;
        }
    }
    Check(e.ComputeHullIndices(), "Lifted corner rebuilds");

    // 검사: 삼각형 수, 닫힌 곡면, 감김, 부피를 확인한다.
    Check(e.VertexData.size() == 8 && e.IndexData.size() == 36, "Lifted corner 12 triangles");
    Check(IsClosedSurface(e) && OutwardWinding(e), "Lifted corner closed and outward");
    CheckNear(e.GetVolume(), 8.0 + 4e-6 / 3.0, 1e-10, "Lifted corner volume");
}

// 1e-9 기울기 평면이 만드는 얕은 면도 병합되지 않고, 모든 정점이 반환 평면 안쪽에 있다.
static void TestTinyTiltFace()
{
    // 준비: 긴 상자에 1e-9 기울어진 평면을 더한다.
    std::vector<Plane> planes = AlignedBox({0,0,0}, {1,100,1});
    const double len = std::sqrt(1.0 + 1e-18);
    planes.push_back({ {1.0 / len, 1e-9 / len, 0}, (1.0 - 5e-10) / len });

    // 동작: hull을 만들고 지지 평면을 꺼낸다.
    ConvexElem e;
    Check(e.HullFromPlanes(planes, {}), "Tiny tilt hull builds");
    std::vector<Plane> back;
    e.GetPlanes(back);

    // 검사: 면 7개가 남고 모든 정점이 반환 평면 안쪽에 있다.
    Check(back.size() == 7 && IsClosedSurface(e) && OutwardWinding(e), "Tiny tilt keeps 7 faces");
    Check(AllVerticesInside(back, e.VertexData, LocalEpsDistance), "All vertices inside returned planes");

    // 동작: 꺼낸 평면으로 hull을 다시 만든다.
    ConvexElem again;
    Check(again.HullFromPlanes(back, e.VertexData), "Tiny tilt round trip builds");

    // 검사: 왕복 후 부피가 같다.
    CheckNear(again.GetVolume(), e.GetVolume(), 1e-9, "Tiny tilt round trip volume");
}

// 큐브 면·변·모서리 방향에 허용치(1e-10 m) 이하로 튀어나온 점은 제거되고, 허용치를 넘는 점은 진짜 정점으로 남는다.
static void TestBumpPoints()
{
    // 준비: 허용치 이하로 튀어나온 점 세 개를 정의한다.
    const Vec3d bumps[3] = { {1 + 9e-11, 0, 0}, {1 + 9e-11, 1, 0}, {1 + 5e-11, 1 + 5e-11, 1 + 5e-11} };

    // 튀어나온 점마다 큐브에 넣어 정리 결과를 검사한다.
    for (const Vec3d& bump : bumps)
    {
        // 동작: 큐브에 튀어나온 점을 넣고 인덱스를 다시 계산한다.
        ConvexElem e;
        Check(e.HullFromPlanes(AlignedBox({0,0,0}, {1,1,1}), {}), "Cube for bump");
        e.VertexData.push_back(bump);
        Check(e.ComputeHullIndices(), "Rebuild with bump");

        // 검사: hull이 닫히고 바깥 감김이며 부피가 같다.
        Check(IsClosedSurface(e) && OutwardWinding(e), "Bump hull closed and outward");
        CheckNear(e.GetVolume(), 8.0, 1e-9, "Bump volume");
    }

    // 면·변 위의 작은 점마다 제거되는지 검사한다.
    for (int i = 0; i < 2; ++i)
    {
        // 동작: 면·변 위의 작은 점을 넣고 인덱스를 다시 계산한다.
        ConvexElem e;
        Check(e.HullFromPlanes(AlignedBox({0,0,0}, {1,1,1}), {}), "Cube for small bump");
        e.VertexData.push_back(bumps[i]);

        // 검사: 작은 점이 제거되어 8정점 12삼각형이 된다.
        Check(e.ComputeHullIndices() && e.VertexData.size() == 8 && e.IndexData.size() == 36, "Small bump removed");
    }

    // 동작: 허용치를 넘는 점을 넣고 인덱스를 다시 계산한다.
    ConvexElem e;
    Check(e.HullFromPlanes(AlignedBox({0,0,0}, {1,1,1}), {}), "Cube for large bump");
    e.VertexData.push_back({ 1 + 2e-10, 0, 0 });

    // 검사: 큰 점은 정점으로 남고 hull이 닫힌 바깥 감김이다.
    Check(e.ComputeHullIndices() && e.VertexData.size() == 9, "Bump above tolerance stays a vertex");
    Check(IsClosedSurface(e) && OutwardWinding(e), "Large bump closed and outward");
}

// ApplyAffine이 큰 스케일·거울·회전 이동에서 정점 수와 삼각형을 유지하는지 검사한다.
static void TestApplyAffine()
{
    // 준비: 한 변 0.004 m 정육면체를 만든다.
    ConvexElem base;
    Check(base.HullFromPlanes(AlignedBox({0,0,0}, {0.002,0.002,0.002}), {}), "Small cube builds");
    const double volume = base.GetVolume();

    // 검사: 스케일 1e8에서도 정점 8개·삼각형 12개가 유지되고 부피는 s^3배다.
    const double big[3][4] = { {1e8,0,0,0}, {0,1e8,0,0}, {0,0,1e8,0} };
    ConvexElem scaled = base;
    scaled.ApplyAffine(big);
    Check(scaled.VertexData.size() == 8 && scaled.IndexData.size() == 36, "Scale 1e8 keeps topology");
    CheckNear(scaled.GetVolume() / volume, 1e24, 1e-6, "Scale 1e8 volume s^3");
    Check(OutwardWinding(scaled) && scaled.ElemBox.IsValid && std::fabs(scaled.ElemBox.Max.x - 2e5) < 1e-3, "Scale keeps winding and box");

    // 검사: 거울 변환은 감김을 뒤집어 부피가 양수로 유지된다.
    const double mirror[3][4] = { {-1,0,0,0}, {0,1,0,0}, {0,0,1,0} };
    ConvexElem mirrored = base;
    mirrored.ApplyAffine(mirror);
    Check(mirrored.IndexData.size() == 36 && OutwardWinding(mirrored), "Mirror flips winding");
    CheckNear(mirrored.GetVolume(), volume, 1e-18, "Mirror volume stays positive");

    // 검사: 회전(z 90도)과 이동 후 부피가 같고 정점이 정확히 대응한다.
    const double rot[3][4] = { {0,-1,0,5}, {1,0,0,-7}, {0,0,1,3} };
    ConvexElem moved = base;
    moved.ApplyAffine(rot);
    CheckNear(moved.GetVolume(), volume, 1e-18, "Rotation volume");
    Check(OutwardWinding(moved), "Rotation winding");
    const Vec3d expected = { 5 - base.VertexData[0].y, -7 + base.VertexData[0].x, 3 + base.VertexData[0].z };
    const Vec3d d = moved.VertexData[0] - expected;
    Check(Dot(d, d) < 1e-24, "Rotation vertex maps");
}

// 모든 검사를 실행하고 실패 시 비정상 종료 코드를 돌려준다.
int main()
{
    // 모든 테스트를 순서대로 실행하고 예외가 나면 실패로 끝낸다.
    try
    {
        TestCube();
        TestDuplicatePlane();
        TestShallowFace();
        TestInteriorAndEdgePoints();
        TestNonPlanarQuad();
        TestTinyTiltFace();
        TestBumpPoints();
        TestSnapAndMerge();
        TestDegenerate();
        TestClipperBoundary();
        TestRotatedAndFarBox();
        TestUnitConsistency();
        TestApplyAffine();
    }
    catch (const std::exception& e)
    {
        // 실패 메시지와 지금까지의 검사 횟수를 출력한다.
        std::printf("FAILED: %s (after %zu checks)\n", e.what(), g_Checks);
        return 1;
    }

    // 통과한 검사 횟수를 출력한다.
    std::printf("ConvexElemTests passed: %zu checks\n", g_Checks);
    return 0;
}
