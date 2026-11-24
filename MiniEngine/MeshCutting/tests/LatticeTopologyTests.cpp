// VS Developer PowerShell, repository root; build output stays outside the repository:
// cl /nologo /utf-8 /std:c++17 /EHsc /W4 /Fo:$env:TEMP/ /Fe:$env:TEMP/LatticeTopologyTests.exe
//    MiniEngine/MeshCutting/tests/LatticeTopologyTests.cpp MiniEngine/Model/LatticeTopology.cpp
#include "../../Model/LatticeTopology.h"
#include <array>
#include <cstdio>
#include <set>
#include <stdexcept>

// Release에서도 실패를 실행 결과로 보고한다.
static void Require(bool condition, const char* message)
{
    // 실패한 조건 이름을 예외로 전달한다.
    if (!condition) throw std::runtime_error(message);
}

// 분리된 띠·section 이음새·반공간별 연결을 CPU만으로 검증한다.
int main()
{
    // 두 삼각형으로 된 띠를 서로 떨어뜨리면 독립 성분이 둘이다.
    try
    {
        SkinCpuSection strip = { { { -1, 0, 0 }, {}, {} }, { { 1, 0, 0 }, {}, {} },
            { { -1, 1, 0 }, {}, {} }, { { 1, 1, 0 }, {}, {} } };
        strip.Indices = { 0, 1, 2, 1, 3, 2 };
        SkinCpuSection distant = strip;
        for (auto& vertex : distant) vertex.Position[1] += 4;
        Require(CountLatticeComponents(BuildLatticeTopology({ strip, distant })) == 2, "separate strips");

        // 같은 위치를 중복 저장한 section 경계도 같은 셀로 용접된다.
        SkinCpuSection left = { strip[0], strip[1], strip[2] };
        SkinCpuSection right = { strip[1], strip[3], strip[2] };
        left.Indices = right.Indices = { 0, 1, 2 };
        const auto topology = BuildLatticeTopology({ left, right });
        Require(CountLatticeComponents(topology) == 1 && topology.CellCount == 4, "section seam");
        Require(topology.Edges.size() == 5 && topology.Dim[0] == 64, "unique edges and resolution");

        // 다른 skin의 로컬 위치를 각 팔레트로 변환하면 같은 모델 위치로 모인다.
        SkinCpuSection shifted = left;
        left.NumJoints = shifted.NumJoints = 2;
        shifted.StartJoint = 2;
        for (auto& vertex : left)
        {
            // 두 본을 섞어 정규화와 팔레트 오프셋을 함께 검사한다.
            vertex.Weights[0] = 100;
            vertex.Weights[1] = 300;
            vertex.Joints[1] = 1;
        }
        for (auto& vertex : shifted)
        {
            // 두 본의 가중 이동량은 (100*7 + 300*11)/400 = 10이다.
            vertex.Position[0] -= 10;
            vertex.Weights[0] = 100;
            vertex.Weights[1] = 300;
            vertex.Joints[1] = 1;
        }
        const LatticeRestTransform identity = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };
        auto move7 = identity, move11 = identity;
        move7[12] = 7;
        move11[12] = 11;
        const std::vector<LatticeRestTransform> transforms = { identity, identity, move7, move11 };
        const auto rest = BuildLatticeTopology({ left, shifted }, 64, transforms);
        Require(rest.VertexCell[0] == rest.VertexCell[1] && rest.CellCount == 3
            && CountLatticeComponents(rest) == 1, "different skin bind spaces weld");
        Require(rest.RestPositions[0] == rest.RestPositions[1], "weighted model rest positions");
        Require(shifted[0].Position[0] == -11, "original skin position preserved");

        // weight 합이 0이면 변환을 적용하지 않고 원래 로컬 위치를 보존한다.
        shifted[0].Weights[0] = shifted[0].Weights[1] = 0;
        const auto zero = BuildLatticeTopology({ shifted }, 64, transforms);
        Require(zero.RestPositions[0][0][0] == shifted[0].Position[0], "zero weight position");

        // x=0 양쪽 정점을 같은 셀에 두고 다른 셀의 같은 side와 연결한다.
        SkinCpuSection crossing = { { { -.1f, 0, 0 }, {}, {} }, { { .1f, 0, 0 }, {}, {} },
            { { -1.2f, 0, 0 }, {}, {} }, { { 1, 0, 0 }, {}, {} } };
        crossing.Indices = { 0, 1, 2, 0, 1, 3 };
        const auto fine = BuildLatticeTopology({ crossing }, 4);
        Require(fine.VertexCell[0][0] == fine.VertexCell[0][1], "both sides share interior cell");
        LatticeUnionFind split(fine.CellCount * 2);
        std::set<uint32_t> active;
        for (size_t i = 0; i < crossing.Indices.size(); i += 3)
            for (uint32_t edge = 0; edge < 3; ++edge)
            {
                // side는 셀에 덮어쓰지 않고 원본 정점마다 평가한다.
                const uint32_t a = crossing.Indices[i + edge];
                const uint32_t b = crossing.Indices[i + (edge + 1) % 3];
                const uint32_t sideA = crossing[a].Position[0] < 0 ? 1 : 0;
                const uint32_t sideB = crossing[b].Position[0] < 0 ? 1 : 0;
                const uint32_t nodeA = LatticeSideNode(fine.VertexCell[0][a], sideA);
                const uint32_t nodeB = LatticeSideNode(fine.VertexCell[0][b], sideB);

                // 실제 점유한 노드만 세고 같은 반공간의 간선만 합친다.
                active.insert(nodeA);
                active.insert(nodeB);
                if (sideA == sideB) split.Unite(nodeA, nodeB);
            }

        // 예약만 된 비점유 노드를 제외하고 성분 수와 각 연결을 검증한다.
        std::set<uint32_t> roots;
        for (uint32_t node : active) roots.insert(split.Find(node));
        const uint32_t shared = fine.VertexCell[0][0];
        Require(roots.size() == 2 && active.size() == 4, "active cell-side components");
        Require(split.Find(LatticeSideNode(shared, 0)) != split.Find(LatticeSideNode(shared, 1)),
            "opposite sides in one cell stay separate");
        Require(split.Find(LatticeSideNode(shared, 1)) == split.Find(LatticeSideNode(fine.VertexCell[0][2], 1))
            && split.Find(LatticeSideNode(shared, 0)) == split.Find(LatticeSideNode(fine.VertexCell[0][3], 0)),
            "same sides across cells unite");

        // 반대 반공간의 떨어진 띠는 작은 B 기여가 있어도 몸통 A 다수결을 따른다.
        for (auto& vertex : strip)
        {
            vertex.Weights[0] = 60000;
            vertex.Weights[1] = 5535;
            vertex.Joints[1] = 1;
        }
        distant = strip;
        for (auto& vertex : distant) vertex.Position[1] += 4;
        const std::vector<SkinCpuSection> strips = { strip, distant };
        const std::vector<std::array<uint32_t, 2>> patch = { {0,0}, {0,1}, {1,0}, {1,1} };
        const auto groups = [](uint32_t joint) { return joint; };
        const auto labels = LabelPatchIslands(BuildLatticeTopology(strips), strips, patch,
            { {1,1,1,1}, {-1,-1,-1,-1} }, groups);
        Require(labels.IslandCount == 2 && labels.Islands[1].Body == 0
            && labels.Islands[1].Scores[0] > labels.Islands[1].Scores[1]
            && labels.TriangleSideBody[2][1] == 0 && labels.TriangleSideBody[2][0] == UINT32_MAX,
            "opposite waist strip belongs to A");

        // 평면을 가로지르는 띠는 양쪽 아일랜드와 삼각형별 몸체 표를 만든다.
        const auto cut = LabelPatchIslands(BuildLatticeTopology({strip}), {strip}, {{0,0}, {0,1}},
            {{-1,1,-1,1}}, groups);
        Require(cut.IslandCount == 2 && cut.Islands[0].Side != cut.Islands[1].Side
            && cut.Islands[0].Triangles == 2 && cut.Islands[1].Triangles == 2, "crossing strip islands");
        const auto onPlane = LabelPatchIslands(BuildLatticeTopology({strip}), {strip}, {{0,0}, {0,1}},
            {{0,0,0,0}}, groups);
        Require(onPlane.IslandCount == 1 && onPlane.TriangleSideBody[0][1] == UINT32_MAX, "coplanar side0");
        // 반대 부호의 두 꼭짓점도 양쪽 투표에 포함되어 같은 B 다수결을 만든다.
        SkinCpuSection voting = strip;
        for (uint32_t v = 0; v < 3; ++v)
        {
            voting[v].Weights[0] = v == 0 ? 100 : 0;
            voting[v].Weights[1] = v == 0 ? 0 : 100;
        }
        const auto fullVote = LabelPatchIslands(BuildLatticeTopology({voting}), {voting}, {{0,0}},
            {{1,-1,-1,1}}, groups);
        Require(fullVote.IslandCount == 2 && fullVote.TriangleSideBody[0][0] == 1
            && fullVote.TriangleSideBody[0][1] == 1, "opposite sign vertices vote on both sides");
        for (const auto& island : fullVote.Islands)
            Require(island.Scores[0] == 1 && island.Scores[1] == 2, "all three bind weights per side");

        // GPU에서 점이나 선분만 남는 side0은 소속·투표·기본 몸체에서 빠진다.
        const auto pointTouch = LabelPatchIslands(BuildLatticeTopology({voting}), {voting}, {{0,0}},
            {{0,-1,-1,1}}, groups);
        const auto edgeTouch = LabelPatchIslands(BuildLatticeTopology({voting}), {voting}, {{0,0}},
            {{0,0,-1,1}}, groups);
        for (const auto* touch : { &pointTouch, &edgeTouch })
        {
            Require(touch->IslandCount == 1 && touch->Islands[0].Side == 1
                && touch->Islands[0].Triangles == 1 && touch->Islands[0].Scores[0] == 1
                && touch->Islands[0].Scores[1] == 2, "touch only votes on side1");
            Require(touch->TriangleSideBody[0][0] == UINT32_MAX && touch->TriangleSideBody[0][1] == 1
                && touch->SideDefault[0] == 0 && touch->SideDefault[1] == 1, "touch excludes empty side0");
        }

        // 세 꼭짓점 모두 동점이면 양쪽에서 A를 우선한다.
        for (auto& vertex : strip) vertex.Weights[0] = vertex.Weights[1] = 100;
        const auto tie = LabelPatchIslands(BuildLatticeTopology({strip}), {strip}, {{0,0}}, {{1,-1,-1,1}}, groups);
        Require(tie.TriangleSideBody[0][0] == 0 && tie.TriangleSideBody[0][1] == 0, "weight tie chooses A");
        const auto tiedBodies = LabelPatchIslands(BuildLatticeTopology({strip}), {strip}, {{0,0}},
            {{1,-1,-1,1}}, [](uint32_t joint) { return joint + 1; });
        Require(tiedBodies.TriangleSideBody[0][0] == 0, "B1 B2 tie chooses A");

        // 평면 위 정점은 교차 띠의 양쪽 연결에 참여하며 두 side는 합쳐지지 않는다.
        const auto zeroCrossing = LabelPatchIslands(BuildLatticeTopology({strip}), {strip}, {{0,0}, {0,1}},
            {{0,-1,1,0}}, groups);
        Require(zeroCrossing.IslandCount == 2 && zeroCrossing.Islands[0].Triangles == 2
            && zeroCrossing.Islands[1].Triangles == 2, "on-plane vertices join both sides");

        // 순서가 섞인 오목 polygon의 내부·외부·경계와 빈 loop를 검사한다.
        const std::vector<std::array<float, 4>> loop = {
            {2,0,1,1}, {0,2,0,0}, {1,1,2,2}, {0,0,2,0}, {2,2,0,2} };
        Require(ContainsLatticeLoop(loop, {0.5f,1}) && !ContainsLatticeLoop(loop, {1.8f,1})
            && ContainsLatticeLoop(loop, {1,1}) && ContainsLatticeLoop(loop, {1,0})
            && !ContainsLatticeLoop({}, {0,0}), "MI3e2 concave containment and boundary");

        // 중첩 loop는 방향과 열거 순서에 무관하게 최소 면적 폐곡선 하나만 선택한다.
        const std::vector<std::array<float, 4>> outer = {
            {-3,-3,3,-3}, {3,3,-3,3}, {3,-3,3,3}, {-3,3,-3,-3} };
        const std::vector<std::array<float, 4>> inner = {
            {-1,-1,-1,1}, {1,1,1,-1}, {-1,1,1,1}, {1,-1,-1,-1} };
        Require(SmallestContainingLatticeLoop({{0,outer}, {7,inner}}, {0,0}) == 7
            && SmallestContainingLatticeLoop({{0,inner}, {7,outer}}, {0,0}) == 0
            && SmallestContainingLatticeLoop({{0,outer}, {7,inner}}, {2,0}) == 0
            && SmallestContainingLatticeLoop({{0,outer}}, {4,0}) == UINT32_MAX,
            "MI3e2 nested loops select only smallest area");

        // 끝점 허용오차는 정확한 끝점으로 고정하고 B는 선분 진행 방향을 따른다.
        float fraction = 0;
        uint32_t childSide = 0;
        Require(IntersectLatticeSegment(-1, 1, fraction, childSide) && fraction == 0.5f && childSide == 0,
            "MI3e2 strict crossing");
        Require(IntersectLatticeSegment(0, -1, fraction, childSide) && fraction == 0 && childSide == 1,
            "MI3e2 parent on plane");
        Require(IntersectLatticeSegment(-1, -1e-5f, fraction, childSide) && fraction == 1 && childSide == 0,
            "MI3e2 child on plane points toward positive");
        Require(IntersectLatticeSegment(1, 1e-5f, fraction, childSide) && fraction == 1 && childSide == 1,
            "MI3e2 child on plane points toward negative");
        Require(IntersectLatticeSegment(1e-5f, 1, fraction, childSide) && fraction == 0 && childSide == 0
            && !IntersectLatticeSegment(2e-5f, 1, fraction, childSide)
            && !IntersectLatticeSegment(0, 0, fraction, childSide), "MI3e2 tolerance and coplanar rejection");

        // 실제 loop의 한 삼각형만 요청해도 아일랜드 전체가 바뀌며 허리는 다수결을 유지한다.
        auto relabeled = LabelPatchIslands(BuildLatticeTopology(strips), strips, patch,
            {{1,1,1,1}, {1,1,1,1}}, groups);
        const auto waist = relabeled.TriangleSideBody.back()[0];
        const auto changed = RelabelPatchIslands(relabeled, {{0,0,1}, {0,0,1}, {999,0,1}, {0,1,1}});
        Require(changed == std::array<uint32_t, 3>{1,2,0} && relabeled.TriangleSideBody[0][0] == 1
            && relabeled.TriangleSideBody[1][0] == 1 && relabeled.TriangleSideBody.back()[0] == waist
            && relabeled.TriangleSideBody[0][1] == UINT32_MAX && relabeled.SideDefault[0] == 0,
            "MI3e2 whole island and untouched waist");

        // 상반된 loop 요구는 최초 다수결을 보존하고 없는 side는 sentinel로 남는다.
        auto conflicting = fullVote;
        const auto conflict = RelabelPatchIslands(conflicting, {{0,0,0}, {0,0,1}, {0,1,0}});
        Require(conflict[2] == 1 && conflicting.TriangleSideBody[0][0] == fullVote.TriangleSideBody[0][0]
            && conflicting.TriangleSideBody[0][1] == 0 && conflicting.SideDefault[1] == 0,
            "MI3e2 conflicting demands preserve majority and update defaults");

        // 뒤집힌 U(П)자 메시를 quad로 만든다. 윗변 한 줄과 아래로 뻗은 두 다리이며 모든 정점이 joint 0이다.
        SkinCpuSection arch;
        for (uint32_t y = 0; y < 4; ++y)
        {
            for (uint32_t x = 0; x < 4; ++x)
            {
                // 4x4 격자 정점을 정수 좌표에 둔다.
                SkinCpuVertex vertex = {};
                vertex.Position[0] = float(x);
                vertex.Position[1] = float(y);
                vertex.Weights[0] = 65535;
                arch.push_back(vertex);
            }
        }
        const std::array<std::array<uint32_t, 2>, 7> archCells = { {
            {0,0}, {0,1}, {2,0}, {2,1}, {0,2}, {1,2}, {2,2} } };
        for (const auto& cell : archCells)
        {
            // 한 칸을 삼각형 두 개로 나눈다.
            const uint32_t v00 = cell[1] * 4 + cell[0];
            const uint32_t v10 = v00 + 1;
            const uint32_t v01 = v00 + 4;
            const uint32_t v11 = v01 + 1;
            for (uint32_t v : { v00, v10, v01, v10, v11, v01 })
            {
                arch.Indices.push_back(v);
            }
        }

        // quad 위상 맵과 모든 삼각형 목록을 만든다.
        const auto archTopology = BuildLatticeTopology({ arch });
        std::vector<std::array<uint32_t, 2>> archTriangles;
        for (uint32_t t = 0; t < arch.Indices.size() / 3; ++t)
        {
            archTriangles.push_back({ 0, t });
        }
        const auto archIslands = [&](float planeY)
        {
            // 수평 평면 y=planeY에 대한 정점 부호로 아일랜드를 센다.
            std::vector<int8_t> archSigns;
            for (const auto& vertex : arch)
            {
                archSigns.push_back(vertex.Position[1] > planeY ? 1 : vertex.Position[1] < planeY ? -1 : 0);
            }
            return LabelPatchIslands(archTopology, { arch }, archTriangles, { archSigns }, groups);
        };

        // 다리를 가로지르면 윗변 쪽 하나와 다리 둘이 따로 떨어진 3개이고 윗변 쪽만 side0이다.
        const auto legsCut = archIslands(1.5f);
        Require(legsCut.IslandCount == 3, "arch legs cut gives three islands");
        uint32_t legSides[2] = {};
        for (const auto& island : legsCut.Islands)
        {
            ++legSides[island.Side];
        }
        Require(legSides[0] == 1 && legSides[1] == 2, "arch bar is one side and legs are the other");

        // 윗변만 가로지르면 윗변 위쪽 하나와 나머지 한 덩어리인 2개다.
        Require(archIslands(2.5f).IslandCount == 2, "arch bar cut gives two islands");

        // 빈 입력과 점 크기 bounds도 유효한 결과를 낸다.
        Require(CountLatticeComponents(BuildLatticeTopology({})) == 0, "empty");
        Require(BuildLatticeTopology({ SkinCpuSection{ strip[0], strip[0] } }).CellCount == 1, "point bounds");
        std::puts("LatticeTopologyTests: PASS (MI1 topology, MI2 waist=A, full triangle votes, point/edge touch=side1, crossing=2, coplanar=side0, tie=A)");
        return 0;
    }
    catch (const std::exception& error)
    {
        // 검사 실패 원인을 표준 출력과 종료 코드에 남긴다.
        std::printf("LatticeTopologyTests: FAIL %s\n", error.what());
        return 1;
    }
}
