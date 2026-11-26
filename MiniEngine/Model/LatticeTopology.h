#pragma once
#include "SkinCpuData.h"
#include <array>
#include <functional>
#include <map>

// DirectX 저장 순서의 affine 행렬이며 RestNodeTransform * IBM을 미리 합성한다.
using LatticeRestTransform = std::array<float, 16>;

// 같은 셀의 양쪽 반공간을 서로 다른 노드로 식별한다. side는 0 또는 1이다.
inline uint32_t LatticeSideNode(uint32_t cell, uint32_t side)
{
    // 셀마다 연속한 두 노드 번호를 예약한다.
    return cell * 2 + side;
}

struct LatticeTopology
{
    float Origin[3] = {};
    float CellSize = 0;
    uint32_t Dim[3] = {};
    uint32_t CellCount = 0;
    std::vector<std::vector<std::array<float, 3>>> RestPositions; // 공통 모델 공간; 원본 skin 위치와 별도
    std::vector<std::vector<uint32_t>> VertexCell;
    std::vector<std::vector<uint32_t>> Indices;
    std::vector<std::array<uint32_t, 2>> Edges;
};

class LatticeUnionFind
{
public:
    // 독립 노드를 만든다. (셀, side)도 정수 id로 넘길 수 있다.
    explicit LatticeUnionFind(uint32_t count);
    // 경로를 압축하며 대표를 찾는다.
    uint32_t Find(uint32_t node);
    // 두 성분을 크기 기준으로 합친다.
    void Unite(uint32_t a, uint32_t b);
    // 현재 독립 성분 수를 읽는다.
    uint32_t Count() const;
private:
    std::vector<uint32_t> m_Parent, m_Size;
    uint32_t m_Count;
};

// 본 팔레트로 모델 공간 rest 위치를 구해 용접한다. 빈 팔레트는 항등 변환이다.
LatticeTopology BuildLatticeTopology(const std::vector<SkinCpuSection>& sections, uint32_t cellsOnLongestAxis = 64,
    const std::vector<LatticeRestTransform>& restTransforms = {});
// 점유 셀과 삼각형 간선으로 전체 연결 성분 수를 센다.
uint32_t CountLatticeComponents(const LatticeTopology& topology);

struct PatchIsland
{
    uint32_t Side = 0, Triangles = 0, Body = 0;
    std::vector<double> Scores = { 0 };
    std::map<uint32_t, double> JointScores; // 모델 팔레트 인덱스별 진단 근거
};
struct IslandLabels
{
    std::vector<std::array<uint32_t, 2>> TriangleSideBody; // 없는 side는 UINT32_MAX
    std::vector<std::array<uint32_t, 2>> TriangleSideIsland; // 재라벨에 사용하는 안정된 아일랜드 번호
    std::vector<PatchIsland> Islands;
    uint32_t IslandCount = 0;
    uint32_t SideDefault[2] = {}; // 해당 side의 삼각형 수 다수결, 동점 A
};
// NDC식 셀 그래프다. 삼각형 표면 표본이 놓인 셀만 노드로 삼고 BuildLatticeTopology 결과와는 별개로 만든다.
struct LatticeCellGraph
{
    std::vector<std::array<uint32_t, 3>> CellCoords;    // 셀 id별 정수 격자 좌표
    std::vector<uint32_t> SampleSection;                // 표본별 section
    std::vector<std::array<uint32_t, 3>> SampleVertices; // 표본별 삼각형 정점 3개(정점 표본은 v,v,v)
    std::vector<std::array<float, 3>> SampleWeights;    // 표본별 무게중심 가중치
    std::vector<uint32_t> SampleCell;                   // 표본별 셀 id
    std::vector<std::vector<uint32_t>> VertexSample;    // section별 정점 자신의 표본 번호
};

// NDC식 셀 그래프 아일랜드 결과다. 삼각형은 쓰지 않고 점유 셀만 노드로 삼는다.
struct CellIslandLabels
{
    std::vector<uint32_t> CellIsland;   // 셀 id별 아일랜드 번호(0..IslandCount-1, 셀 수 내림차순)
    std::vector<uint32_t> IslandCells;  // 아일랜드별 셀 수
    std::vector<int8_t> IslandSide;     // 아일랜드별 side(-1 또는 +1)
    uint32_t IslandCount = 0;
};

// 삼각형 표면을 CellSize/2 이하 간격으로 표집해 점유 셀 그래프를 만든다.
LatticeCellGraph BuildLatticeCellGraph(const LatticeTopology& topology);
// 표본 거리를 세 정점 거리의 보간으로 구해 셀 평균 부호를 side로 쓴다. |평균| <= 1e-5이면 0이다.
std::vector<int8_t> CellSidesFromVertexDistances(const LatticeCellGraph& graph,
    const std::vector<std::vector<float>>& vertexDistances);
// 점유 셀의 격자 인접(6 또는 26 이웃) 중 같은 side끼리만 이어 연결 성분을 아일랜드로 만든다. side 0은 +1로 취급한다.
CellIslandLabels LabelLatticeCellIslands(const LatticeCellGraph& graph, const std::vector<int8_t>& cellSides,
    uint32_t neighborhood);
// 표본별 아일랜드 번호를 반환한다.
std::vector<uint32_t> SampleIslands(const LatticeCellGraph& graph, const CellIslandLabels& labels);
// 정점 자신의 표본이 속한 셀의 아일랜드를 section별 정점 순서로 반환한다.
std::vector<std::vector<uint32_t>> VertexIslands(const LatticeCellGraph& graph, const CellIslandLabels& labels);

// 패치 삼각형의 같은 반공간 셀을 연결하고 bind weight 다수결로 몸체를 정한다.
IslandLabels LabelPatchIslands(const LatticeTopology& topology, const std::vector<SkinCpuSection>& sections,
    const std::vector<std::array<uint32_t, 2>>& triangles, const std::vector<std::vector<int8_t>>& signs,
    const std::function<uint32_t(uint32_t)>& jointGroup);

struct PatchIslandVote
{
    uint32_t Body = 0;
    uint32_t Margin = 0;
    bool Near = false;
    bool Fallback = false;
};
// 기존 섬 번호로 uint16 원시 weight를 정수 정규화한다. 런타임에서는 아직 호출하지 않는다.
std::vector<PatchIslandVote> VotePatchIslandsInteger(const LatticeTopology& topology,
    const std::vector<SkinCpuSection>& sections, const std::vector<std::array<uint32_t, 2>>& triangles,
    const IslandLabels& oracle, const std::function<uint32_t(uint32_t)>& jointGroup);
// 근접 또는 overflow 섬만 double oracle을 선택하고 몸체 표와 side 기본값을 재구성한다.
IslandLabels ResolvePatchIslandVotes(const IslandLabels& oracle, const std::vector<PatchIslandVote>& votes);

// 순서와 무관한 폐곡선 선분의 홀짝 교차로 내부와 경계 포함을 판정한다.
bool ContainsLatticeLoop(const std::vector<std::array<float, 4>>& edges, const std::array<float, 2>& point);
// 교차점을 감싼 유효 폐곡선 중 최소 면적 loop를 반환한다.
uint32_t SmallestContainingLatticeLoop(const std::map<uint32_t, std::vector<std::array<float, 4>>>& loops,
    const std::array<float, 2>& point);
// 끝점 허용오차를 처리하고 부모에서 자식으로 향하는 반공간을 반환한다.
bool IntersectLatticeSegment(float parentDistance, float childDistance, float& fraction, uint32_t& childSide);
// (삼각형, side, 요구 몸체)를 아일랜드에 전파하고 충돌은 원래 다수결을 보존한다.
std::array<uint32_t, 3> RelabelPatchIslands(IslandLabels& labels,
    const std::vector<std::array<uint32_t, 3>>& requests);
