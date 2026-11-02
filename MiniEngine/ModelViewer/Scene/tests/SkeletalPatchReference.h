#pragma once
#include "../../../MeshCutting/Geometry/CutPlane.h"
#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Viewer::PatchReference
{
    constexpr uint32_t InvalidNode = UINT32_MAX;
    struct Vertex
    {
        Math::Vector3 Position;
        std::array<uint16_t, 4> Palette;
        std::array<uint16_t, 4> Weights;
    };
    struct Draw { uint32_t StartIndex, IndexCount; int32_t BaseVertex; };
    struct Section
    {
        std::vector<Vertex> Vertices;
        std::vector<uint16_t> Indices16;
        std::vector<uint32_t> Indices32; // R32일 때만 사용하고, 그 외에는 R16을 사용한다.
        std::vector<Draw> Draws;
        uint32_t StartJoint = 0;
    };
    struct Input
    {
        std::vector<Section> Sections;
        std::vector<uint32_t> JointNodes;
        MeshCutting::CutPlaneData Plane; // World 필드 이름과 무관하게 모두 component-local이다.
        float Epsilon = 1e-5f;
    };
    struct Triangle
    {
        uint32_t SectionId, DrawId, TriangleId;
        std::array<uint32_t, 3> Vertices;
    };

    // draw 시작 인덱스와 baseVertex를 적용해 원본 꼭짓점 번호를 얻는다.
    inline std::array<uint32_t, 3> TriangleIndices(const Section& section, const Draw& draw, uint32_t triangle)
    {
        // R16/R32의 동일한 논리 인덱스를 component 꼭짓점 번호로 변환한다.
        std::array<uint32_t, 3> indices;
        for (uint32_t corner = 0; corner < 3; ++corner)
        {
            const uint32_t offset = draw.StartIndex + triangle * 3 + corner;
            const uint32_t index = section.Indices32.empty() ? section.Indices16.at(offset) : section.Indices32.at(offset);
            indices[corner] = uint32_t(int64_t(index) + draw.BaseVertex);
        }
        return indices;
    }

    // 엄격한 양쪽 부호와 유한 사각형 겹침으로 교차 삼각형을 판정한다.
    inline bool CrossesRectangle(const Input& input, const Section& section, const std::array<uint32_t, 3>& indices)
    {
        // eps 안의 꼭짓점은 평면 위로 투영하고 부호 거리를 0으로 만든다.
        Math::Vector3 points[3], hits[3];
        float distances[3];
        for (uint32_t i = 0; i < 3; ++i)
        {
            points[i] = section.Vertices.at(indices[i]).Position;
            const float distance = float(Math::Dot(Math::Vector4(points[i], 1), input.Plane.WorldPlane));
            distances[i] = std::abs(distance) <= input.Epsilon ? 0.0f : distance;
            if (distances[i] == 0) points[i] -= Math::Vector3(input.Plane.WorldPlane) * distance;
        }
        if (*std::min_element(distances, distances + 3) >= -input.Epsilon
            || *std::max_element(distances, distances + 3) <= input.Epsilon) return false;

        // 평면 위 꼭짓점과 반대 부호 변의 교점으로 교차 선분을 만든다.
        uint32_t count = 0;
        for (uint32_t i = 0; i < 3; ++i)
        {
            const uint32_t j = (i + 1) % 3;
            if (distances[i] == 0) hits[count++] = points[i];
            if (distances[i] * distances[j] < 0)
                hits[count++] = points[i] + (points[j] - points[i]) * (distances[i] / (distances[i] - distances[j]));
        }
        float t0, t1;
        return count == 2 && MeshCutting::ClipCoplanarSegmentToCutPlaneRectangle(input.Plane, hits[0], hits[1], t0, t1);
    }

    // 선택 본에 양수 weight가 있는 삼각형 전체를 원본 순서로 추출한다.
    inline std::vector<Triangle> ExtractPatchReference(const Input& input, uint32_t node)
    {
        // section/draw/triangle 순서와 원본 꼭짓점 번호를 함께 보존한다.
        std::vector<Triangle> result;
        if (node == InvalidNode) return result;
        for (uint32_t s = 0; s < input.Sections.size(); ++s)
            for (uint32_t d = 0; d < input.Sections[s].Draws.size(); ++d)
                for (uint32_t t = 0; t < input.Sections[s].Draws[d].IndexCount / 3; ++t)
                {
                    const auto& section = input.Sections[s];
                    const auto indices = TriangleIndices(section, section.Draws[d], t);
                    bool include = false;
                    for (uint32_t index : indices)
                        for (uint32_t slot = 0; slot < 4; ++slot)
                            if (section.Vertices.at(index).Weights[slot] > 0
                                && input.JointNodes.at(section.StartJoint + section.Vertices.at(index).Palette[slot]) == node)
                                include = true;
                    if (include) result.push_back({ s, d, t, indices });
                }
        return result;
    }
}
