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

// 삼각형 하나당 분할 수 상한이다. 표본 수는 삼각형당 약 n^2/2개로 늘어난다.
static const uint32_t MaxSubdivision = 128;

// 삼각형 표면을 CellSize/2 이하 간격으로 표집해 점유 셀 그래프를 만든다. 정점 하나당 표본 하나를 먼저 둔다.
LatticeCellGraph BuildLatticeCellGraph(const LatticeTopology& topology)
{
    // 섹션이 없는 빈 위상은 표집할 것이 없어 셀 크기 0도 허용한다. 그 외 0 이하·비유한 셀 크기는 거절한다.
    if (!topology.RestPositions.empty() && (!(topology.CellSize > 0) || !std::isfinite(topology.CellSize)))
    {
        throw std::invalid_argument("lattice cell graph cell size");
    }

    LatticeCellGraph graph;
    std::unordered_map<uint64_t, uint32_t> cellIds;

    // 표본 위치를 BuildLatticeTopology와 같은 식으로 셀 id에 배정한다.
    const auto cellOf = [&](const std::array<float, 3>& position)
    {
        // 축마다 셀 좌표를 구하고 격자 밖은 마지막 칸으로 모은다.
        uint64_t key = 0;
        std::array<uint32_t, 3> coords = {};
        for (uint32_t axis = 0; axis < 3; ++axis)
        {
            const double offset = double(position[axis]) - topology.Origin[axis];
            const uint32_t rawCoord = uint32_t(offset / topology.CellSize);
            coords[axis] = std::min(topology.Dim[axis] - 1, rawCoord);
            key = key * 65536 + coords[axis];
        }

        // 처음 보는 셀이면 새 id를 주고 좌표를 기록한다.
        const auto entry = cellIds.emplace(key, uint32_t(cellIds.size()));
        if (entry.second)
        {
            graph.CellCoords.push_back(coords);
        }
        return entry.first->second;
    };

    // 표본 하나를 모든 배열에 함께 추가한다.
    const auto addSample = [&](uint32_t section, const std::array<uint32_t, 3>& vertices,
        const std::array<float, 3>& weights)
    {
        std::array<float, 3> position = {};
        for (uint32_t k = 0; k < 3; ++k)
        {
            for (uint32_t axis = 0; axis < 3; ++axis)
            {
                position[axis] += weights[k] * topology.RestPositions[section][vertices[k]][axis];
            }
        }
        graph.SampleSection.push_back(section);
        graph.SampleVertices.push_back(vertices);
        graph.SampleWeights.push_back(weights);
        graph.SampleCell.push_back(cellOf(position));
    };

    // 모든 정점에 자기 표본(가중치 1,0,0)을 하나씩 만든다.
    for (uint32_t s = 0; s < topology.RestPositions.size(); ++s)
    {
        graph.VertexSample.emplace_back();
        for (uint32_t v = 0; v < topology.RestPositions[s].size(); ++v)
        {
            graph.VertexSample.back().push_back(uint32_t(graph.SampleCell.size()));
            addSample(s, { v, v, v }, { 1, 0, 0 });
        }
    }

    // 삼각형마다 가장 긴 변을 CellSize/2로 나눈 n단계 무게중심 격자를 표집한다.
    for (uint32_t s = 0; s < topology.Indices.size(); ++s)
    {
        const auto& positions = topology.RestPositions[s];
        for (size_t t = 0; t + 2 < topology.Indices[s].size(); t += 3)
        {
            // 인덱스 범위를 검사하고 세 변 중 가장 긴 길이를 구한다.
            const std::array<uint32_t, 3> tri = { topology.Indices[s][t], topology.Indices[s][t + 1],
                topology.Indices[s][t + 2] };
            double longest = 0;
            for (uint32_t k = 0; k < 3; ++k)
            {
                if (tri[k] >= positions.size())
                {
                    throw std::invalid_argument("lattice cell graph index");
                }
                double lengthSq = 0;
                for (uint32_t axis = 0; axis < 3; ++axis)
                {
                    const double d = double(positions[tri[k]][axis]) - positions[tri[(k + 1) % 3]][axis];
                    lengthSq += d * d;
                }
                longest = std::max(longest, std::sqrt(lengthSq));
            }
            // 거대 삼각형이 표본을 폭증시키지 않도록 분할 수를 상한으로 제한한다.
            const uint32_t n = std::min(MaxSubdivision,
                std::max(1u, uint32_t(std::ceil(longest / (topology.CellSize * 0.5)))));

            // 세 꼭짓점을 뺀 격자점(i, j)을 가중치 (n-i-j, i, j)/n으로 추가한다.
            for (uint32_t i = 0; i <= n; ++i)
            {
                for (uint32_t j = 0; i + j <= n; ++j)
                {
                    const bool corner = (i == 0 && j == 0) || (i == n && j == 0) || (i == 0 && j == n);
                    if (corner)
                    {
                        continue;
                    }
                    addSample(s, tri, { float(n - i - j) / n, float(i) / n, float(j) / n });
                }
            }
        }
    }
    return graph;
}

// 표본 거리를 세 정점 거리의 무게중심 보간으로 구하고 셀 평균의 부호를 side로 쓴다. |평균| <= 1e-5이면 0이다.
std::vector<int8_t> CellSidesFromVertexDistances(const LatticeCellGraph& graph,
    const std::vector<std::vector<float>>& vertexDistances)
{
    // 거리 배열이 정점 수와 정확히 같은지 검사한다.
    if (vertexDistances.size() != graph.VertexSample.size())
    {
        throw std::invalid_argument("lattice distance sections");
    }
    for (size_t s = 0; s < vertexDistances.size(); ++s)
    {
        if (vertexDistances[s].size() != graph.VertexSample[s].size())
        {
            throw std::invalid_argument("lattice distance vertices");
        }
    }

    // 표본별 보간 거리를 셀별 합과 표본 수로 누적한다.
    std::vector<double> sums(graph.CellCoords.size(), 0.0);
    std::vector<uint32_t> counts(graph.CellCoords.size(), 0);
    for (size_t k = 0; k < graph.SampleCell.size(); ++k)
    {
        const auto& distances = vertexDistances[graph.SampleSection[k]];
        double distance = 0;
        for (uint32_t c = 0; c < 3; ++c)
        {
            distance += double(graph.SampleWeights[k][c]) * distances[graph.SampleVertices[k][c]];
        }
        sums[graph.SampleCell[k]] += distance;
        ++counts[graph.SampleCell[k]];
    }

    // 평균의 부호를 side로 변환한다.
    std::vector<int8_t> sides(graph.CellCoords.size(), 0);
    for (size_t cell = 0; cell < sides.size(); ++cell)
    {
        const double average = counts[cell] ? sums[cell] / counts[cell] : 0.0;
        int side = 0;
        if (std::abs(average) > 1e-5)
        {
            side = average > 0 ? 1 : -1;
        }
        sides[cell] = static_cast<int8_t>(side);
    }
    return sides;
}

// 점유 셀 사이의 격자 인접 중 같은 side끼리만 이어 연결 성분을 아일랜드로 만든다.
// side 0(미정)은 +1로 취급한다. neighborhood는 6(면) 또는 26(면+모서리+꼭짓점)이다.
CellIslandLabels LabelLatticeCellIslands(const LatticeCellGraph& graph, const std::vector<int8_t>& cellSides,
    uint32_t neighborhood)
{
    // 입력 크기와 이웃 종류를 검사한다.
    if (neighborhood != 6 && neighborhood != 26)
    {
        throw std::invalid_argument("lattice neighborhood");
    }
    const uint32_t cellCount = uint32_t(graph.CellCoords.size());
    if (cellSides.size() != cellCount)
    {
        throw std::invalid_argument("lattice cell sides");
    }

    // 격자 좌표에서 셀 id를 찾는 표를 만든다.
    const auto makeKey = [](int64_t x, int64_t y, int64_t z)
    {
        return uint64_t((x * 65536 + y) * 65536 + z);
    };
    std::unordered_map<uint64_t, uint32_t> lookup;
    for (uint32_t cell = 0; cell < cellCount; ++cell)
    {
        const auto& c = graph.CellCoords[cell];
        lookup.emplace(makeKey(c[0], c[1], c[2]), cell);
    }

    // 이웃 셀이 점유되어 있고 side가 같으면 연결한다. side 0은 +1로 본다.
    LatticeUnionFind links(cellCount);
    for (uint32_t cell = 0; cell < cellCount; ++cell)
    {
        const auto& c = graph.CellCoords[cell];
        const bool sideA = cellSides[cell] >= 0;
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dz = -1; dz <= 1; ++dz)
                {
                    // 이웃 종류에 맞는 오프셋만 쓴다.
                    const int nonZero = (dx != 0) + (dy != 0) + (dz != 0);
                    if (nonZero == 0 || (neighborhood == 6 && nonZero != 1))
                    {
                        continue;
                    }

                    // 격자 범위를 벗어나는 음수 좌표를 건너뛴다.
                    const int64_t nx = int64_t(c[0]) + dx;
                    const int64_t ny = int64_t(c[1]) + dy;
                    const int64_t nz = int64_t(c[2]) + dz;
                    if (nx < 0 || ny < 0 || nz < 0)
                    {
                        continue;
                    }

                    // 점유된 이웃 셀을 찾는다.
                    const auto found = lookup.find(makeKey(nx, ny, nz));
                    if (found == lookup.end())
                    {
                        continue;
                    }

                    // side가 같을 때만 합친다.
                    if ((cellSides[found->second] >= 0) == sideA)
                    {
                        links.Unite(cell, found->second);
                    }
                }
            }
        }
    }

    // 대표별 셀 수와 최소 셀 id를 모으고 셀 수 내림차순(동률은 최소 셀 id 순)으로 정렬한다.
    std::map<uint32_t, std::array<uint32_t, 2>> roots;
    for (uint32_t cell = 0; cell < cellCount; ++cell)
    {
        auto& entry = roots.emplace(links.Find(cell), std::array<uint32_t, 2>{ 0, cell }).first->second;
        ++entry[0];
    }
    std::vector<std::pair<uint32_t, std::array<uint32_t, 2>>> ordered(roots.begin(), roots.end());
    std::stable_sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b)
    {
        if (a.second[0] != b.second[0])
        {
            return a.second[0] > b.second[0];
        }
        return a.second[1] < b.second[1];
    });

    // 정렬된 순서대로 번호를 부여하고 셀별 아일랜드 번호를 채운다.
    CellIslandLabels result;
    result.CellIsland.assign(cellCount, 0);
    std::unordered_map<uint32_t, uint32_t> rootToIsland;
    for (const auto& item : ordered)
    {
        rootToIsland.emplace(item.first, uint32_t(result.IslandCells.size()));
        result.IslandCells.push_back(item.second[0]);
        result.IslandSide.push_back(cellSides[item.second[1]] >= 0 ? 1 : -1);
    }
    for (uint32_t cell = 0; cell < cellCount; ++cell)
    {
        result.CellIsland[cell] = rootToIsland.at(links.Find(cell));
    }
    result.IslandCount = uint32_t(result.IslandCells.size());
    return result;
}

// 표본마다 그 표본이 속한 셀의 아일랜드 번호를 반환한다.
std::vector<uint32_t> SampleIslands(const LatticeCellGraph& graph, const CellIslandLabels& labels)
{
    // 표본의 셀 번호로 아일랜드를 조회한다.
    std::vector<uint32_t> islands;
    for (uint32_t cell : graph.SampleCell)
    {
        islands.push_back(labels.CellIsland.at(cell));
    }
    return islands;
}

// 각 원본 정점의 아일랜드를 정점 자신의 표본(가중치 1,0,0)이 속한 셀에서 구한다.
std::vector<std::vector<uint32_t>> VertexIslands(const LatticeCellGraph& graph, const CellIslandLabels& labels)
{
    // 정점 표본 번호를 따라 아일랜드를 조회한다.
    std::vector<std::vector<uint32_t>> islands;
    for (const auto& samples : graph.VertexSample)
    {
        islands.emplace_back();
        for (uint32_t sample : samples)
        {
            islands.back().push_back(labels.CellIsland.at(graph.SampleCell[sample]));
        }
    }
    return islands;
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
    result.TriangleSideIsland = nodes;
    result.TriangleSideBody = nodes;
    for (auto& bodies : result.TriangleSideBody)
        for (auto& body : bodies)
            if (body != UINT32_MAX) body = result.Islands[body].Body;
    return result;
}

// 경계는 포함하고 수평 변과 꼭짓점의 중복 교차는 반개구간으로 처리한다.
bool ContainsLatticeLoop(const std::vector<std::array<float, 4>>& edges, const std::array<float, 2>& point)
{
    // 세 변 미만은 유효한 폐곡선이 아니다.
    if (edges.size() < 3) return false;

    // 각 선분의 방향과 검사점의 상대 위치를 계산한다.
    bool inside = false;
    for (const auto& e : edges)
    {
        // 경계 판정에 사용할 길이와 외적 및 내적을 구한다.
        const double dx = double(e[2]) - e[0], dy = double(e[3]) - e[1];
        const double x = double(point[0]) - e[0], y = double(point[1]) - e[1];
        const double length = dx * dx + dy * dy;
        const double cross = x * dy - y * dx, dot = x * dx + y * dy;

        // 선분 경계 위의 점은 폐곡선 내부에 포함한다.
        if (length > 0 && cross * cross <= 1e-12 * length && dot >= 0 && dot <= length) return true;

        // 반개구간 교차마다 홀짝 상태를 반전한다.
        if ((e[1] > point[1]) != (e[3] > point[1]) && x < dx * y / dy) inside = !inside;
    }

    // 모든 선분을 지난 뒤의 홀짝 상태를 반환한다.
    return inside;
}

// 순서가 섞인 유향 변의 면적을 비교해 중첩 loop 중 가장 안쪽 하나를 고른다.
uint32_t SmallestContainingLatticeLoop(const std::map<uint32_t, std::vector<std::array<float, 4>>>& loops,
    const std::array<float, 2>& point)
{
    // 폐곡선의 포함 여부를 확인하고 최소 절대 면적 후보만 남긴다.
    uint32_t selected = UINT32_MAX;
    double smallest = std::numeric_limits<double>::max();
    for (const auto& loop : loops)
    {
        if (!ContainsLatticeLoop(loop.second, point)) continue;
        double area = 0;
        for (const auto& edge : loop.second)
        {
            area += double(edge[0]) * edge[3] - double(edge[2]) * edge[1];
        }

        // 호출자가 유효성을 확인한 폐곡선에서 퇴화 면적은 제외한다.
        area = std::abs(area);
        if (area > 0 && area < smallest)
        {
            smallest = area;
            selected = loop.first;
        }
    }
    return selected;
}

// 평면 끝점 허용오차와 엄격한 관통을 같은 선분 교차 규칙으로 처리한다.
bool IntersectLatticeSegment(float parentDistance, float childDistance, float& fraction, uint32_t& childSide)
{
    // 양 끝이 평면 위이면 유일한 교차점이 없으므로 제외한다.
    const bool parentOn = std::abs(parentDistance) <= 1e-5f;
    const bool childOn = std::abs(childDistance) <= 1e-5f;
    if (parentOn && childOn) return false;
    if (!parentOn && !childOn && (parentDistance > 0) == (childDistance > 0)) return false;

    // 끝점 교차는 끝점에 고정하고 B는 부모에서 자식으로 향하는 방향으로 정한다.
    fraction = parentOn ? 0.0f : childOn ? 1.0f : parentDistance / (parentDistance - childDistance);
    childSide = childDistance > parentDistance ? 0 : 1;
    return true;
}

// 모든 요구를 먼저 모아 순서에 무관하게 충돌 아일랜드의 다수결을 보존한다.
std::array<uint32_t, 3> RelabelPatchIslands(IslandLabels& labels,
    const std::vector<std::array<uint32_t, 3>>& requests)
{
    // 같은 아일랜드에 다른 몸체 요구가 오면 별도 충돌 표시에 둔다.
    std::vector<uint32_t> bodies(labels.Islands.size(), UINT32_MAX);
    std::vector<bool> conflict(bodies.size(), false);
    for (const auto& request : requests)
    {
        if (request[0] >= labels.TriangleSideIsland.size() || request[1] > 1) continue;
        const uint32_t island = labels.TriangleSideIsland[request[0]][request[1]];
        if (island == UINT32_MAX) continue;
        if (bodies[island] != UINT32_MAX && bodies[island] != request[2]) conflict[island] = true;
        bodies[island] = request[2];
    }

    // 변경된 아일랜드 수와 삼각형-side 수 및 충돌 수를 반환한다.
    std::array<uint32_t, 3> counts = {};
    for (size_t i = 0; i < bodies.size(); ++i)
    {
        if (conflict[i]) ++counts[2];
        else if (bodies[i] != UINT32_MAX && labels.Islands[i].Body != bodies[i])
        {
            labels.Islands[i].Body = bodies[i];
            ++counts[0];
            counts[1] += labels.Islands[i].Triangles;
        }
    }

    // GPU 표와 cap 임시 기본 몸체를 재라벨된 결과로 다시 계산한다.
    std::map<uint32_t, uint32_t> totals[2];
    for (size_t t = 0; t < labels.TriangleSideBody.size(); ++t)
        for (uint32_t side = 0; side < 2; ++side)
        {
            const uint32_t island = labels.TriangleSideIsland[t][side];
            if (island == UINT32_MAX) continue;
            const uint32_t body = labels.Islands[island].Body;
            labels.TriangleSideBody[t][side] = body;
            ++totals[side][body];
        }
    for (uint32_t side = 0; side < 2; ++side)
    {
        labels.SideDefault[side] = 0;
        uint32_t best = 0;
        for (const auto& total : totals[side])
            if (total.second > best)
            {
                best = total.second;
                labels.SideDefault[side] = total.first;
            }
    }
    return counts;
}
