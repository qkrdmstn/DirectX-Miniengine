#pragma once
#include "CollisionUnits.h"
#include <cmath>
#include <cstdint>
#include <vector>

namespace MiniCollision
{
    // 3차원 double 벡터.
    struct Vec3d
    {
        double x;
        double y;
        double z;
    };

    // 두 벡터의 합을 돌려준다.
    inline Vec3d operator+(Vec3d a, Vec3d b)
    {
        // 성분별 합을 새 벡터로 돌려준다.
        return { a.x + b.x, a.y + b.y, a.z + b.z };
    }

    // 두 벡터의 차를 돌려준다.
    inline Vec3d operator-(Vec3d a, Vec3d b)
    {
        // 성분별 차를 새 벡터로 돌려준다.
        return { a.x - b.x, a.y - b.y, a.z - b.z };
    }

    // 벡터에 스칼라를 곱한다.
    inline Vec3d operator*(Vec3d a, double s)
    {
        // 각 성분에 스칼라를 곱한 벡터를 돌려준다.
        return { a.x * s, a.y * s, a.z * s };
    }

    // 두 벡터의 내적을 돌려준다.
    inline double Dot(Vec3d a, Vec3d b)
    {
        // 성분별 곱의 합을 돌려준다.
        return a.x * b.x + a.y * b.y + a.z * b.z;
    }

    // 두 벡터의 외적을 돌려준다.
    inline Vec3d Cross(Vec3d a, Vec3d b)
    {
        // 오른손 법칙의 외적 성분을 계산해 돌려준다.
        return {
            a.y * b.z - a.z * b.y,
            a.z * b.x - a.x * b.z,
            a.x * b.y - a.y * b.x
        };
    }

    // Unreal FPlane과 같은 관례: PlaneDot(P) = Dot(Normal, P) - W. 법선은 바깥, 안쪽은 PlaneDot <= 0.
    struct Plane
    {
        Vec3d Normal;
        double W;

        // 점이 평면에서 법선 방향으로 떨어진 부호 있는 거리를 돌려준다.
        double PlaneDot(Vec3d p) const
        {
            // 법선과의 내적에서 상수항을 뺀 값을 돌려준다.
            return Dot(Normal, p) - W;
        }
    };

    // 정점 경계 상자. 비어 있으면 IsValid가 false다.
    struct Box
    {
        Vec3d Min{ 0, 0, 0 };
        Vec3d Max{ 0, 0, 0 };
        bool IsValid = false;
    };

    // 허용 오차 묶음. 기본값은 m 단위 상수이며 테스트에서 cm 단위 Unreal 값을 넣어 단위 일치를 검증한다.
    struct HullTolerances
    {
        double MinVertDistance = MinHullVertDistance;
        double MinArea = MinHullArea;
        double BigHalfExtent = BigPolygonHalfExtent;
        double EpsAngle = LocalEpsAngle;
        double EpsDistance = LocalEpsDistance;
    };

    // 다각형을 평면 안쪽(PlaneDot <= eps) 반쪽만 남기도록 자른다. 평면 위(|d| <= eps) 점은 안쪽으로 유지한다.
    std::vector<Vec3d> ClipPolygonInside(const std::vector<Vec3d>& poly, const Plane& plane, double eps);

    // 평면마다 큰 사각형을 나머지 평면으로 잘라 남은 다각형 정점을 snap·병합 없이 out에 모은다(EpsDistance 안의 중복만 제거). HullFromPlanes와 같은 절단 경로를 쓴다.
    void CollectPlanePolygonVertices(const std::vector<Plane>& planes, const HullTolerances& tol, std::vector<Vec3d>& out);

    // Unreal FKConvexElem 중 정점·삼각형·평면 교집합 기능만 옮긴 볼록 요소다. VertexData가 기준(인덱스 재구성 후에는 hull 모서리 정점만 남음)이고 삼각형은 바깥에서 볼 때 반시계다.
    struct ConvexElem
    {
        std::vector<Vec3d> VertexData;
        std::vector<uint32_t> IndexData;
        Box ElemBox;

        // 정점·인덱스·경계 상자를 모두 비운다.
        void Reset();

        // 정점의 최소·최대 좌표로 ElemBox를 다시 계산하고 인덱스를 재구성한다.
        void UpdateElemBox(double faceTol = LocalEpsDistance);

        // 중복 정점을 정리하고 볼록 껍질 면을 삼각화해 닫힌 곡면 IndexData를 만든다. 실패하면 IndexData를 비우고 false를 돌려준다.
        // 전제: VertexData는 HullFromPlanes 결과이거나 그 정확한 복사본이고, 정점끼리 서로 약 MinHullVertDistance(0.001 m) 이상 떨어져 있어야 한다.
        // 이보다 가까운 입력은 성공을 보장하지 않는다(false와 빈 IndexData로 실패할 뿐, 잘못된 곡면을 성공으로 보고하지는 않는다).
        bool ComputeHullIndices(double faceTol = LocalEpsDistance);

        // 바깥 법선 평면들의 안쪽 교집합 hull을 만든다. 실패하면 요소를 비우고 false를 돌려준다.
        bool HullFromPlanes(
            const std::vector<Plane>& planes,
            const std::vector<Vec3d>& snapVerts,
            double snapDistSq = DefaultSnapDistSq, const HullTolerances& tol = HullTolerances());

        // 재구성된 면마다 지지 평면 하나를 out에 추가한다.
        void GetPlanes(std::vector<Plane>& out, double faceTol = LocalEpsDistance) const;

        // 삼각형 인덱스로 hull 부피를 구한다. 바깥 감김이면 양수다.
        double GetVolume() const;
    };
}
