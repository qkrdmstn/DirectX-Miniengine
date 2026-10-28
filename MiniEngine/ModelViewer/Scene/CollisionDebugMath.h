#pragma once

#include "../../Physics/Collision/ConvexElem.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace Viewer
{
    namespace CollisionDebug
    {
        // 인덱스로 가리키는 hull 모서리 하나다. A < B로 정렬해 방향 없는 쌍으로 다룬다.
        struct HullEdge
        {
            uint32_t A;
            uint32_t B;
        };

        // 카메라 뷰 공간 점이다. 카메라 정면은 -Z이다.
        struct ViewPoint
        {
            float X;
            float Y;
            float Z;
        };

        // 면 법선 내적이 1 - 이 값보다 크면 같은 평면으로 보고 모서리를 숨긴다. 약 0.0026도 미만의 꺾임이 숨는다.
        constexpr double CoplanarDotTolerance = 1.0e-9;

        // 삼각형 한 변의 정렬된 끝점 쌍과 그 삼각형의 단위 면 법선(퇴화면 Valid=false)을 담은 기록이다.
        struct EdgeRecord
        {
            uint32_t A;
            uint32_t B;
            bool Valid;
            double Nx;
            double Ny;
            double Nz;
        };

        // 인덱스 삼각형에서 방향 없는 모서리를 모으되, 유효한 인접 면이 정확히 둘이고 법선이 사실상 같은 모서리는 숨긴다.
        // 퇴화 삼각형은 유효한 면 수에 세지 않으므로 그 변은 숨겨지지 않고 그려진다.
        inline void ExtractHullEdges(const std::vector<MiniCollision::Vec3d>& vertices,
            const std::vector<uint32_t>& indices, std::vector<HullEdge>& edges)
        {
            // 삼각형마다 세 변의 기록을 만든다.
            edges.clear();
            std::vector<EdgeRecord> records;
            records.reserve(indices.size());
            for (size_t t = 0; t + 2 < indices.size(); t += 3)
            {
                const uint32_t i0 = indices[t];
                const uint32_t i1 = indices[t + 1];
                const uint32_t i2 = indices[t + 2];
                if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
                {
                    continue;
                }

                // 배정밀도 외적으로 면 법선을 구하고 상대 크기가 너무 작으면 퇴화로 본다.
                const MiniCollision::Vec3d e1 = vertices[i1] - vertices[i0];
                const MiniCollision::Vec3d e2 = vertices[i2] - vertices[i0];
                const MiniCollision::Vec3d e3 = vertices[i2] - vertices[i1];
                const double nx = e1.y * e2.z - e1.z * e2.y;
                const double ny = e1.z * e2.x - e1.x * e2.z;
                const double nz = e1.x * e2.y - e1.y * e2.x;
                const double length = std::sqrt(nx * nx + ny * ny + nz * nz);
                const double longest = std::max({ MiniCollision::Dot(e1, e1), MiniCollision::Dot(e2, e2), MiniCollision::Dot(e3, e3) });
                const bool valid = std::isfinite(length) && length > 0.0 && length > 1.0e-12 * longest;
                const double ux = valid ? nx / length : 0.0;
                const double uy = valid ? ny / length : 0.0;
                const double uz = valid ? nz / length : 0.0;

                // 변을 작은 인덱스가 앞에 오도록 기록하고 길이 0인 변은 버린다.
                const uint32_t corner[3] = { i0, i1, i2 };
                for (int k = 0; k < 3; ++k)
                {
                    const uint32_t a = corner[k];
                    const uint32_t b = corner[(k + 1) % 3];
                    if (a == b)
                    {
                        continue;
                    }
                    records.push_back({ std::min(a, b), std::max(a, b), valid, ux, uy, uz });
                }
            }

            // 같은 쌍끼리 모이도록 정렬한다.
            std::sort(records.begin(), records.end(), [](const EdgeRecord& l, const EdgeRecord& r)
            {
                return l.A != r.A ? l.A < r.A : l.B < r.B;
            });

            // 쌍마다 유효한 면 법선을 모아 숨길지 판단하고 남은 모서리를 출력한다.
            size_t begin = 0;
            while (begin < records.size())
            {
                size_t end = begin;
                size_t validCount = 0;
                const EdgeRecord* first = nullptr;
                const EdgeRecord* second = nullptr;
                while (end < records.size() && records[end].A == records[begin].A && records[end].B == records[begin].B)
                {
                    if (records[end].Valid)
                    {
                        ++validCount;
                        if (!first)
                        {
                            first = &records[end];
                        }
                        else if (!second)
                        {
                            second = &records[end];
                        }
                    }
                    ++end;
                }

                // 유효한 면이 정확히 둘이고 거의 같은 방향이면 삼각분할 대각선으로 보고 숨긴다.
                bool hidden = false;
                if (validCount == 2)
                {
                    const double dot = first->Nx * second->Nx + first->Ny * second->Ny + first->Nz * second->Nz;
                    hidden = dot > 1.0 - CoplanarDotTolerance;
                }
                if (!hidden)
                {
                    edges.push_back({ records[begin].A, records[begin].B });
                }
                begin = end;
            }
        }

        // 선분을 근평면 앞(z <= -nearClip)으로 자른다. 완전히 뒤이면 false이고, 걸치면 뒤쪽 끝점을 교차점으로 바꾼다.
        inline bool ClipSegmentToNearPlane(ViewPoint& a, ViewPoint& b, float nearClip)
        {
            // 각 끝점의 앞쪽 여유를 구한다. 0 이상이면 평면 위 또는 앞이다.
            const float da = -nearClip - a.Z;
            const float db = -nearClip - b.Z;
            if (da < 0.0f && db < 0.0f)
            {
                return false;
            }
            if (da >= 0.0f && db >= 0.0f)
            {
                return true;
            }

            // 걸치면 평면과의 교차점으로 뒤쪽 끝점을 옮긴다.
            const float t = da / (da - db);
            const ViewPoint cut = { a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t, -nearClip };
            if (da < 0.0f)
            {
                a = cut;
            }
            else
            {
                b = cut;
            }
            return true;
        }
    }
}
