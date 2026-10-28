// Run from a VS Developer PowerShell at the repository root (outputs outside the repository):
// cl /nologo /std:c++17 /EHsc /W4 /I MiniEngine/Core /I MiniEngine/Model /I MiniEngine/Physics
//    /Fo:<external-directory>\ /Fe:<external-directory>/CollisionDebugTests.exe
//    MiniEngine/ModelViewer/Scene/tests/CollisionDebugTests.cpp
//    MiniEngine/Physics/Collision/ConvexElem.cpp MiniEngine/Physics/Collision/AggregateGeom.cpp

#include "../CollisionDebugMath.h"
#include "../GizmoMath.h"
#include "../../../Physics/Collision/AggregateGeom.h"
#include <cstdio>
#include <set>
#include <stdexcept>

using namespace MiniCollision;
using namespace Viewer::CollisionDebug;

namespace
{
    int Checks = 0;

    // 조건이 거짓이면 예외로 실패를 알린다.
    void Check(bool condition, const char* description)
    {
        // 검사 횟수를 세고 실패하면 예외를 던진다.
        ++Checks;
        if (!condition)
        {
            throw std::runtime_error(description);
        }
    }

    // 축 정렬 상자의 바깥 법선 6평면을 만든다.
    std::vector<Plane> BoxPlanes(double half)
    {
        // 법선과 거리 쌍으로 여섯 면을 나열한다.
        return {
            { { 1, 0, 0 }, half }, { { -1, 0, 0 }, half }, { { 0, 1, 0 }, half },
            { { 0, -1, 0 }, half }, { { 0, 0, 1 }, half }, { { 0, 0, -1 }, half } };
    }

    // 평면 목록에서 hull과 삼각형 인덱스를 만든다.
    ConvexElem MakeHull(const std::vector<Plane>& planes)
    {
        // 평면에서 hull을 만든다.
        ConvexElem elem;
        Check(elem.HullFromPlanes(planes, {}), "hull builds");

        // 삼각형 인덱스가 만들어졌는지 확인하고 돌려준다.
        Check(!elem.IndexData.empty(), "hull has triangle indices");
        return elem;
    }

    // hull에서 숨김 규칙을 적용한 모서리 수를 센다.
    size_t CountEdges(const ConvexElem& elem)
    {
        // 모서리를 추출해 개수를 돌려준다.
        std::vector<HullEdge> edges;
        ExtractHullEdges(elem.VertexData, elem.IndexData, edges);
        return edges.size();
    }

    // 단위 상자는 대각선이 숨겨져 모서리 12개만 남아야 한다.
    void CubeTest()
    {
        // 단위 상자 hull을 만든다.
        const ConvexElem cube = MakeHull(BoxPlanes(0.5));

        // 정점 8개와 모서리 12개를 확인한다.
        Check(cube.VertexData.size() == 8, "cube has 8 vertices");
        Check(CountEdges(cube) == 12, "unit cube yields exactly 12 edges");
    }

    // 윗면이 약 0.5도로 꺾인 상자는 꺾임 모서리를 유지해 15개가 남아야 한다.
    void ShallowCreaseTest()
    {
        // 0.5도 기울어진 두 윗면 평면으로 hull을 만든다.
        const double angle = 0.5 * 3.14159265358979323846 / 180.0;
        const double s = std::sin(angle);
        const double c = std::cos(angle);
        std::vector<Plane> planes = BoxPlanes(0.5);
        planes.erase(planes.begin() + 2);
        planes.push_back({ { -s, c, 0 }, -s * -0.5 + c * 0.5 });
        planes.push_back({ { s, c, 0 }, s * 0.5 + c * 0.5 });

        // 모서리를 추출해 15개인지 확인한다.
        const ConvexElem hull = MakeHull(planes);
        std::vector<HullEdge> edges;
        ExtractHullEdges(hull.VertexData, hull.IndexData, edges);
        Check(edges.size() == 15, "shallow crease box keeps 15 edges");

        // 능선(x=0, 위쪽 두 정점)을 잇는 모서리가 있어야 한다.
        bool ridge = false;
        for (const HullEdge& e : edges)
        {
            const Vec3d& a = hull.VertexData[e.A];
            const Vec3d& b = hull.VertexData[e.B];
            ridge |= std::fabs(a.x) < 1.0e-9 && std::fabs(b.x) < 1.0e-9;
        }
        Check(ridge, "crease ridge edge is kept");
    }

    // 상자를 평면으로 자른 두 조각의 모서리 수를 검사한다.
    void CutPieceTest()
    {
        // 한 변이 2인 상자를 x=0 평면으로 자른다.
        AggregateGeom cube;
        cube.ConvexElems.push_back(MakeHull(BoxPlanes(1.0)));
        AggregateGeom keep;
        AggregateGeom other;
        SliceAggregateGeom(cube, { { 1, 0, 0 }, 0.0 }, keep, other);

        // 두 조각이 각각 상자 모서리 12개를 갖는지 확인한다.
        Check(keep.ConvexElems.size() == 1 && other.ConvexElems.size() == 1, "axis cut yields two pieces");
        Check(CountEdges(keep.ConvexElems[0]) == 12 && CountEdges(other.ConvexElems[0]) == 12, "axis cut pieces are boxes with 12 edges");

        // 대각 평면 절단은 삼각기둥 두 개이므로 각각 9개다.
        const double r = std::sqrt(0.5);
        AggregateGeom keep2;
        AggregateGeom other2;
        SliceAggregateGeom(cube, { { r, r, 0 }, 0.0 }, keep2, other2);
        Check(keep2.ConvexElems.size() == 1 && other2.ConvexElems.size() == 1, "diagonal cut yields two pieces");
        Check(CountEdges(keep2.ConvexElems[0]) == 9 && CountEdges(other2.ConvexElems[0]) == 9, "diagonal cut prisms have 9 edges");
    }

    // 퇴화 삼각형이 섞여도 그 변의 유효 면이 둘이 아니면 숨기지 않는지 검사한다.
    void DegenerateTest()
    {
        // 한 직선 위의 퇴화 삼각형을 섞은 정점과 인덱스를 준비한다.
        const std::vector<Vec3d> v = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 }, { 2, 0, 0 } };
        std::vector<HullEdge> edges;
        ExtractHullEdges(v, { 0, 1, 2, 0, 1, 3 }, edges);

        // 퇴화 삼각형의 변도 남는지 확인한다.
        Check(edges.size() == 5, "degenerate triangle keeps its edges");

        // 범위 밖 인덱스 삼각형은 건너뛰는지 확인한다.
        ExtractHullEdges(v, { 0, 1, 2, 0, 1, 99 }, edges);
        Check(edges.size() == 3, "out-of-range triangle is skipped");
    }

    // 근평면 자르기의 걸침·완전 뒤·평면 위 경우와 투영 좌표의 유한성을 검사한다.
    void NearClipTest()
    {
        // 근평면과 투영 행렬, 화면 좌표 유한성 검사기를 준비한다.
        const float nearClip = 0.1f;
        const DirectX::XMMATRIX xm = DirectX::XMMatrixPerspectiveFovRH(DirectX::XM_PIDIV4, 16.0f / 9.0f, nearClip, 1000.0f);
        const Math::Matrix4 projection(xm);
        const auto finite = [&](const ViewPoint& p)
        {
            Viewer::GizmoMath::Point screen;
            return Viewer::GizmoMath::Project(Math::Vector3(p.X, p.Y, p.Z), projection, 1920.0f, 1080.0f, screen) &&
                std::isfinite(screen.X) && std::isfinite(screen.Y);
        };

        // 걸치는 선분은 교차점이 z=-near에 놓이고 유한한 화면 좌표를 만든다.
        ViewPoint a = { 0.5f, 0.2f, -5.0f };
        ViewPoint b = { 1.0f, 0.4f, 3.0f };
        Check(ClipSegmentToNearPlane(a, b, nearClip), "crossing segment is kept");
        Check(a.Z == -5.0f && std::fabs(b.Z + nearClip) < 1.0e-6f, "behind endpoint moves to the near plane");
        Check(finite(a) && finite(b), "clipped crossing segment projects to finite coordinates");

        // 방향을 바꿔도 같은 결과이다.
        ViewPoint c = { 1.0f, 0.4f, 3.0f };
        ViewPoint d = { 0.5f, 0.2f, -5.0f };
        Check(ClipSegmentToNearPlane(c, d, nearClip) && std::fabs(c.Z + nearClip) < 1.0e-6f, "reversed crossing is kept");

        // 완전히 뒤쪽이면 버린다.
        ViewPoint e = { 0, 0, 1.0f };
        ViewPoint f = { 1, 1, 0.5f };
        Check(!ClipSegmentToNearPlane(e, f, nearClip), "fully behind segment is rejected");

        // 근평면 위 끝점은 앞쪽으로 보고 그대로 유지한다.
        ViewPoint g = { 0, 0, -nearClip };
        ViewPoint h = { 1, 0, -2.0f };
        Check(ClipSegmentToNearPlane(g, h, nearClip) && g.Z == -nearClip && h.Z == -2.0f, "on-plane endpoint is kept unchanged");

        // 평면 위에 놓인 선분은 그대로 유지한다.
        ViewPoint i = { 0, 0, -nearClip };
        ViewPoint j = { 1, 0, -nearClip };
        Check(ClipSegmentToNearPlane(i, j, nearClip) && finite(i) && finite(j), "segment lying on the plane is kept");

        // 한 끝이 평면 위이고 다른 끝이 뒤면 한 점으로 줄어든다(길이 0이라 그려지지 않는다).
        ViewPoint k = { 0, 0, -nearClip };
        ViewPoint l = { 1, 0, 2.0f };
        Check(ClipSegmentToNearPlane(k, l, nearClip) && std::fabs(l.Z + nearClip) < 1.0e-6f && l.X == 0.0f, "touching segment collapses to the plane point");
    }
}

// 충돌 디버그 표시용 순수 계산의 CPU 검사를 실행한다.
int main()
{
    // 검사를 차례로 실행한다.
    try
    {
        CubeTest();
        ShallowCreaseTest();
        CutPieceTest();
        DegenerateTest();
        NearClipTest();
        std::printf("CollisionDebugTests: %d checks passed\n", Checks);
        return 0;
    }
    // 실패하면 이유를 출력하고 비정상 종료한다.
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "CollisionDebugTests failed: %s\n", error.what());
        return 1;
    }
}
