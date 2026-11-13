#pragma once
#include "SkinCpuData.h"
#include <array>

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
