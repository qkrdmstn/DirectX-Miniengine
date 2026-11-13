// VS Developer PowerShell, repository root; build output stays outside the repository:
// cl /nologo /utf-8 /std:c++17 /EHsc /W4 /Fo:$env:TEMP/ /Fe:$env:TEMP/LatticeTopologyTests.exe
//    MiniEngine/MeshCutting/tests/LatticeTopologyTests.cpp MiniEngine/Model/LatticeTopology.cpp
#include "../../Model/LatticeTopology.h"
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

        // 빈 입력과 점 크기 bounds도 유효한 결과를 낸다.
        Require(CountLatticeComponents(BuildLatticeTopology({})) == 0, "empty");
        Require(BuildLatticeTopology({ SkinCpuSection{ strip[0], strip[0] } }).CellCount == 1, "point bounds");
        std::puts("LatticeTopologyTests: PASS (separate=2, seam=1, rest-weld=1, zero-weight, shared-cell split=2, empty, point)");
        return 0;
    }
    catch (const std::exception& error)
    {
        // 검사 실패 원인을 표준 출력과 종료 코드에 남긴다.
        std::printf("LatticeTopologyTests: FAIL %s\n", error.what());
        return 1;
    }
}
