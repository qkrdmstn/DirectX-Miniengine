#pragma once
#include "Collision/ConvexElem.h"
#include <cmath>
#include <map>
#include <stdexcept>
#include <utility>

// ConvexElemTests와 AggregateGeomTests가 함께 쓰는 검사 하네스와 상자·곡면 보조 함수다.
using namespace MiniCollision;
inline size_t g_Checks = 0;

// Release에서도 실패를 예외로 보고한다.
inline void Check(bool condition, const char* message)
{
    // 검사 횟수를 센다.
    ++g_Checks;

    // 조건이 거짓이면 예외로 실패를 알린다.
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

// 두 값이 허용 오차 안에서 같은지 검사한다.
inline void CheckNear(double a, double b, double tol, const char* message)
{
    // 두 값의 차이가 허용 오차 이내인지 검사한다.
    Check(std::fabs(a - b) <= tol, message);
}

// 중심, 세 축, 반크기로 바깥 법선 6평면 상자를 만든다.
inline std::vector<Plane> BoxPlanes(Vec3d c, const Vec3d axes[3], Vec3d half)
{
    // 축별 반크기를 배열로 옮겨 담는다.
    const double h[3] = { half.x, half.y, half.z };

    // 축마다 양의 방향과 음의 방향 평면을 한 쌍씩 추가한다.
    std::vector<Plane> planes;
    for (int i = 0; i < 3; ++i)
    {
        planes.push_back({ axes[i], Dot(axes[i], c) + h[i] });
        planes.push_back({ axes[i] * -1.0, -Dot(axes[i], c) + h[i] });
    }

    // 만들어진 평면 목록을 돌려준다.
    return planes;
}

// 축 정렬 상자 평면을 만든다.
inline std::vector<Plane> AlignedBox(Vec3d c, Vec3d half)
{
    // 월드 축 세 개로 상자 평면을 만든다.
    const Vec3d axes[3] = { {1,0,0}, {0,1,0}, {0,0,1} };
    return BoxPlanes(c, axes, half);
}

// 모든 방향 변이 정확히 한 번씩이고 반대 방향 변이 짝으로 있는 닫힌 곡면인지 검사한다.
inline bool IsClosedSurface(const ConvexElem& e)
{
    // 삼각형의 방향 변 횟수를 센다.
    std::map<std::pair<uint32_t, uint32_t>, int> edges;
    for (size_t t = 0; t + 2 < e.IndexData.size(); t += 3)
    {
        for (size_t k = 0; k < 3; ++k)
        {
            ++edges[{ e.IndexData[t + k], e.IndexData[t + (k + 1) % 3] }];
        }
    }

    // 모든 변이 한 번씩이고 반대 방향 짝이 있는지 확인한다.
    for (const auto& entry : edges)
    {
        const auto back = edges.find({ entry.first.second, entry.first.first });
        if (entry.second != 1 || back == edges.end() || back->second != 1)
        {
            return false;
        }
    }

    // 변이 하나라도 있고 모두 짝이 맞으면 닫힌 곡면이다.
    return !edges.empty();
}

