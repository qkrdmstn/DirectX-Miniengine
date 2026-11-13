#include "LatticeTopology.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <unordered_map>

// 지정한 수의 독립 노드를 초기화한다.
LatticeUnionFind::LatticeUnionFind(uint32_t count) : m_Parent(count), m_Size(count, 1), m_Count(count)
{
    // 최초에는 각 노드 자신이 대표다.
    std::iota(m_Parent.begin(), m_Parent.end(), 0u);
}

// 대표까지의 경로를 절반씩 압축한다.
uint32_t LatticeUnionFind::Find(uint32_t node)
{
    // 대표에 도달할 때까지 조부모로 건너뛴다.
    while (node != m_Parent[node])
    {
        m_Parent[node] = m_Parent[m_Parent[node]];
        node = m_Parent[node];
    }
    return node;
}

// 작은 트리를 큰 트리에 붙이고 성분 수를 갱신한다.
void LatticeUnionFind::Unite(uint32_t a, uint32_t b)
{
    // 같은 대표라면 이미 연결된 노드다.
    a = Find(a);
    b = Find(b);
    if (a == b) return;

    // 크기 기준 병합으로 트리 깊이를 제한한다.
    if (m_Size[a] < m_Size[b]) std::swap(a, b);
    m_Parent[b] = a;
    m_Size[a] += m_Size[b];
    --m_Count;
}

// 독립 성분 수를 반환한다.
uint32_t LatticeUnionFind::Count() const
{
    // 병합 때 유지한 값을 그대로 사용한다.
    return m_Count;
}

// 전체 section의 bind bounds를 공유하는 희소 격자를 만든다.
LatticeTopology BuildLatticeTopology(const std::vector<SkinCpuSection>& sections, uint32_t cellsOnLongestAxis,
    const std::vector<LatticeRestTransform>& restTransforms)
{
    // 잘못된 해상도와 비유한 위치는 명시적으로 거절한다.
    if (!cellsOnLongestAxis || cellsOnLongestAxis > 65536) throw std::invalid_argument("lattice resolution");
    LatticeTopology result;
    const float limit = std::numeric_limits<float>::max();
    float upper[3] = { -limit, -limit, -limit };
    std::fill_n(result.Origin, 3, limit);
    size_t vertexCount = 0;
    for (const auto& section : sections)
    {
        // 원본 위치와 별도로 각 정점의 모델 공간 rest 위치를 저장한다.
        result.RestPositions.emplace_back();
        for (const auto& vertex : section)
        {
            std::array<float, 3> position = { vertex.Position[0], vertex.Position[1], vertex.Position[2] };
            uint32_t weightSum = 0;
            for (uint32_t k = 0; k < 4; ++k) weightSum += vertex.Weights[k];
            if (weightSum && !restTransforms.empty())
            {
                // 압축 weight를 합으로 정규화해 각 본의 rest 변환을 가중 합산한다.
                position = {};
                for (uint32_t k = 0; k < 4; ++k)
                {
                    if (!vertex.Weights[k]) continue;
                    if (vertex.Joints[k] >= section.NumJoints)
                        throw std::invalid_argument("lattice joint");
                    const auto& matrix = restTransforms.at(size_t(section.StartJoint) + vertex.Joints[k]);
                    const float weight = float(vertex.Weights[k]) / weightSum;
                    for (uint32_t axis = 0; axis < 3; ++axis)
                        position[axis] += weight * (matrix[axis] * vertex.Position[0]
                            + matrix[4 + axis] * vertex.Position[1]
                            + matrix[8 + axis] * vertex.Position[2] + matrix[12 + axis]);
                }
            }

            // 변환 결과로만 bounds를 확장한다. weight 합 0은 원본 위치다.
            result.RestPositions.back().push_back(position);
            ++vertexCount;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                if (!std::isfinite(position[axis])) throw std::invalid_argument("lattice position");
                result.Origin[axis] = std::min(result.Origin[axis], position[axis]);
                upper[axis] = std::max(upper[axis], position[axis]);
            }
        }
    }
    if (!vertexCount) return LatticeTopology{};

    // 최장축에 맞춘 동일 셀 크기를 쓰며 최대 경계는 마지막 셀에 넣는다.
    double longest = 0;
    for (uint32_t axis = 0; axis < 3; ++axis)
        longest = std::max(longest, double(upper[axis]) - result.Origin[axis]);
    result.CellSize = longest > 0 ? float(longest / cellsOnLongestAxis) : 1.0f;
    for (uint32_t axis = 0; axis < 3; ++axis)
        result.Dim[axis] = std::max(1u, std::min(cellsOnLongestAxis,
            uint32_t(std::ceil((double(upper[axis]) - result.Origin[axis]) / result.CellSize))));

    // section 경계와 무관하게 동일 격자 좌표에 같은 압축 id를 배정한다.
    std::unordered_map<uint64_t, uint32_t> cells;
    for (size_t sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex)
    {
        const auto& section = sections[sectionIndex];
        result.VertexCell.emplace_back();
        auto& vertexCells = result.VertexCell.back();
        for (const auto& position : result.RestPositions[sectionIndex])
        {
            uint64_t key = 0;
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                const uint32_t coord = std::min(result.Dim[axis] - 1,
                    uint32_t((double(position[axis]) - result.Origin[axis]) / result.CellSize));
                key = key * 65536 + coord;
            }
            vertexCells.push_back(cells.emplace(key, uint32_t(cells.size())).first->second);
        }

        // 원본 삼각형 순서를 보존하고 각 삼각형의 서로 다른 셀 쌍을 연결한다.
        result.Indices.push_back(section.Indices);
        if (section.Indices.size() % 3) throw std::invalid_argument("lattice triangles");
        for (size_t i = 0; i < section.Indices.size(); i += 3)
            for (uint32_t edge = 0; edge < 3; ++edge)
            {
                uint32_t a = vertexCells.at(section.Indices[i + edge]);
                uint32_t b = vertexCells.at(section.Indices[i + (edge + 1) % 3]);
                if (a > b) std::swap(a, b);
                if (a != b) result.Edges.push_back({ a, b });
            }
    }

    // 무방향 간선을 정렬해 삼각형과 section 사이의 중복을 제거한다.
    result.CellCount = uint32_t(cells.size());
    std::sort(result.Edges.begin(), result.Edges.end());
    result.Edges.erase(std::unique(result.Edges.begin(), result.Edges.end()), result.Edges.end());
    return result;
}

// 격자 전체의 연결 성분 수를 계산한다.
uint32_t CountLatticeComponents(const LatticeTopology& topology)
{
    // 점유 셀만 노드로 만들고 모든 간선을 병합한다.
    LatticeUnionFind components(topology.CellCount);
    for (const auto& edge : topology.Edges) components.Unite(edge[0], edge[1]);
    return components.Count();
}

// 평면 위 꼭짓점은 양쪽 연결에 쓰며 완전 공면 삼각형은 GPU처럼 side0에만 둔다.
IslandLabels LabelPatchIslands(const LatticeTopology& topology, const std::vector<SkinCpuSection>& sections,
    const std::vector<std::array<uint32_t, 2>>& triangles, const std::vector<std::vector<int8_t>>& signs,
    const std::function<uint32_t(uint32_t)>& jointGroup)
{
    // 삼각형별 side 대표를 보관하고 같은 side의 꼭짓점 셀만 병합한다.
    LatticeUnionFind links(topology.CellCount * 2);
    std::vector<std::array<uint32_t, 2>> nodes(triangles.size(), { UINT32_MAX, UINT32_MAX });
    for (size_t t = 0; t < triangles.size(); ++t)
    {
        const uint32_t s = triangles[t][0], tri = triangles[t][1];
        const auto& indices = topology.Indices.at(s);
        bool positive = false, negative = false;
        for (uint32_t k = 0; k < 3; ++k)
        {
            const int sign = signs.at(s).at(indices.at(tri * 3 + k));
            positive |= sign > 0;
            negative |= sign < 0;
        }

        // GPU의 공면 특례를 따르고 점·선분만 남는 side는 소속에서 제외한다.
        for (uint32_t side = 0; side < 2; ++side)
        {
            if (side == 0 ? !positive && negative : !negative) continue;
            for (uint32_t k = 0; k < 3; ++k)
            {
                const uint32_t v = indices.at(tri * 3 + k);
                const int sign = signs.at(s).at(v);
                if (side == 0 ? sign < 0 : sign > 0) continue;
                const uint32_t node = LatticeSideNode(topology.VertexCell.at(s).at(v), side);
                if (nodes[t][side] != UINT32_MAX) links.Unite(nodes[t][side], node);
                nodes[t][side] = node;
            }
        }
    }

    // 활성 대표만 아일랜드로 만들고 소속된 원본 삼각형의 세 꼭짓점 weight를 누적한다.
    IslandLabels result;
    std::map<uint32_t, uint32_t> islands;
    for (size_t t = 0; t < triangles.size(); ++t)
        for (uint32_t side = 0; side < 2; ++side)
        {
            if (nodes[t][side] == UINT32_MAX) continue;
            const uint32_t root = links.Find(nodes[t][side]);
            const auto entry = islands.emplace(root, uint32_t(result.Islands.size()));
            if (entry.second) result.Islands.emplace_back();
            nodes[t][side] = entry.first->second;
            auto& island = result.Islands[nodes[t][side]];
            island.Side = side;
            ++island.Triangles;
            const uint32_t s = triangles[t][0], tri = triangles[t][1];
            for (uint32_t k = 0; k < 3; ++k)
            {
                const uint32_t v = topology.Indices.at(s).at(tri * 3 + k);
                const auto& vertex = sections.at(s).at(v);
                uint32_t sum = 0;
                for (uint32_t weight : vertex.Weights) sum += weight;
                for (uint32_t j = 0; j < 4; ++j)
                {
                    if (!vertex.Weights[j]) continue;
                    const uint32_t joint = sections[s].StartJoint + vertex.Joints[j];
                    const uint32_t group = jointGroup(joint);
                    if (island.Scores.size() <= group) island.Scores.resize(size_t(group) + 1);
                    const double weight = double(vertex.Weights[j]) / sum;
                    island.Scores[group] += weight;
                    island.JointScores[joint] += weight;
                }
            }
        }

    // 동점에는 A를 우선하고 side 기본 몸체는 원본 패치 삼각형 수로 정한다.
    std::map<uint32_t, uint32_t> counts[2];
    result.IslandCount = uint32_t(result.Islands.size());
    for (auto& island : result.Islands)
    {
        for (uint32_t group = 1; group < island.Scores.size(); ++group)
            if (island.Scores[group] > island.Scores[island.Body]) island.Body = group;
        if (std::count(island.Scores.begin(), island.Scores.end(), island.Scores[island.Body]) > 1) island.Body = 0;
        counts[island.Side][island.Body] += island.Triangles;
    }
    for (uint32_t side = 0; side < 2; ++side)
        for (const auto& count : counts[side])
            if (count.second > counts[side][result.SideDefault[side]]) result.SideDefault[side] = count.first;
    result.TriangleSideBody = nodes;
    for (auto& bodies : result.TriangleSideBody)
        for (auto& body : bodies)
            if (body != UINT32_MAX) body = result.Islands[body].Body;
    return result;
}
