#include "ConvexElem.h"
#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <utility>

namespace MiniCollision
{
    namespace
    {
        // 정점 집합에서 만든 볼록 껍질의 한 면: 바깥 법선, 상수항, 면 위에 놓인 정점 인덱스들과 반시계 경계 다각형 인덱스.
        struct Face
        {
            Vec3d Normal;
            double W;
            std::vector<uint32_t> Points;
            std::vector<uint32_t> Boundary;
        };

        // 평면 법선에 수직인 단위 축 X, Y를 만들며 Cross(X, Y)가 법선과 같도록 오른손 계를 이룬다.
        void MakeAxes(Vec3d n, Vec3d& axisX, Vec3d& axisY)
        {
            // 법선과 평행하지 않은 보조 축을 고른다.
            const Vec3d helper = std::fabs(n.z) < 0.9 ? Vec3d{ 0, 0, 1 } : Vec3d{ 1, 0, 0 };

            // 보조 축과 법선의 외적을 정규화해 X 축으로 삼는다.
            axisX = Cross(helper, n);
            axisX = axisX * (1.0 / std::sqrt(Dot(axisX, axisX)));

            // 법선과 X 축의 외적으로 Y 축을 만든다.
            axisY = Cross(n, axisX);
        }

        // 다각형 면적을 정점 외적 합의 절반으로 구한다.
        double PolygonArea(const std::vector<Vec3d>& poly)
        {
            // 첫 정점을 기준으로 삼각 fan 외적을 누적한다.
            Vec3d sum{ 0, 0, 0 };
            for (size_t i = 1; i + 1 < poly.size(); ++i)
            {
                sum = sum + Cross(poly[i] - poly[0], poly[i + 1] - poly[0]);
            }

            // 누적 벡터 길이의 절반이 면적이다.
            return 0.5 * std::sqrt(Dot(sum, sum));
        }

        // 병합 거리 안에 기존 정점이 있으면 그 인덱스를, 없으면 새 정점을 추가해 인덱스를 돌려준다.
        uint32_t AddVertexIfNotPresent(std::vector<Vec3d>& verts, Vec3d v, double minDist)
        {
            // 병합 거리 안에 있는 기존 정점을 찾으면 그 인덱스를 돌려준다.
            for (size_t i = 0; i < verts.size(); ++i)
            {
                const Vec3d d = v - verts[i];
                if (Dot(d, d) < minDist * minDist)
                {
                    return static_cast<uint32_t>(i);
                }
            }

            // 없으면 새 정점을 추가하고 그 인덱스를 돌려준다.
            verts.push_back(v);
            return static_cast<uint32_t>(verts.size() - 1);
        }

        // 가장 가까운 snap 정점이 제곱거리 snapDistSq보다 가까우면 그 정점을, 아니면 원래 정점을 돌려준다.
        Vec3d SnapVertex(Vec3d v, const std::vector<Vec3d>& snapVerts, double snapDistSq)
        {
            // 가장 가까운 snap 정점을 찾는다.
            double nearestSq = std::numeric_limits<double>::max();
            Vec3d nearest = v;
            for (const Vec3d& s : snapVerts)
            {
                const double d = Dot(v - s, v - s);
                if (d < nearestSq)
                {
                    nearestSq = d;
                    nearest = s;
                }
            }

            // 임계 거리 안이면 snap 정점을, 아니면 원래 정점을 돌려준다.
            return nearestSq < snapDistSq ? nearest : v;
        }

        // i번째 평면 위의 큰 사각형을 나머지 모든 평면의 안쪽으로 잘라 남은 다각형을 돌려준다.
        std::vector<Vec3d> ClipPlanePolygon(const std::vector<Plane>& planes, size_t i, const HullTolerances& tol)
        {
            // 평면 위에 놓인 큰 사각형을 만든다.
            Vec3d axisX;
            Vec3d axisY;
            MakeAxes(planes[i].Normal, axisX, axisY);
            const Vec3d base = planes[i].Normal * planes[i].W;
            const Vec3d x = axisX * tol.BigHalfExtent;
            const Vec3d y = axisY * tol.BigHalfExtent;
            std::vector<Vec3d> poly = { base + x + y, base - x + y, base - x - y, base + x - y };

            // 다른 모든 평면의 안쪽으로 사각형을 자른다.
            for (size_t j = 0; j < planes.size() && poly.size() >= 3; ++j)
            {
                if (i != j)
                {
                    poly = ClipPolygonInside(poly, planes[j], tol.EpsDistance);
                }
            }

            // 잘린 다각형을 돌려준다.
            return poly;
        }

        // 면 평면 위 점들을 평면 좌표로 투영해 볼록 경계 다각형(반시계)을 구한다. 내부점·중복점·허용치 안의 공선점은 버린다.
        std::vector<uint32_t> FaceBoundary(const std::vector<Vec3d>& v, const Face& face, double tol)
        {
            // 평면 좌표로 투영한 점과 원래 정점 인덱스를 함께 담는다.
            struct Point2
            {
                double x;
                double y;
                uint32_t index;
            };

            // 면 법선에 수직인 2D 축을 만든다.
            Vec3d axisX;
            Vec3d axisY;
            MakeAxes(face.Normal, axisX, axisY);

            // 면 위 점들을 2D 평면 좌표로 투영한다.
            std::vector<Point2> pts;
            for (uint32_t i : face.Points)
            {
                pts.push_back({ Dot(v[i], axisX), Dot(v[i], axisY), i });
            }

            // 모노톤 체인 입력을 위해 x, y 순으로 정렬한다.
            std::sort(pts.begin(), pts.end(), [](const Point2& a, const Point2& b)
            {
                return a.x < b.x || (a.x == b.x && a.y < b.y);
            });

            // o→a→b가 시계 방향이거나 a가 직선 o→b에서 tol 이내면 a를 버려야 하는지 판정한다.
            auto dropMiddle = [tol](const Point2& o, const Point2& a, const Point2& b)
            {
                const double cross = (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
                return cross <= tol * std::sqrt((b.x - o.x) * (b.x - o.x) + (b.y - o.y) * (b.y - o.y));
            };

            // 아래쪽 껍질을 쌓는다.
            std::vector<Point2> hull;
            for (const Point2& p : pts)
            {
                while (hull.size() >= 2 && dropMiddle(hull[hull.size() - 2], hull[hull.size() - 1], p))
                {
                    hull.pop_back();
                }
                hull.push_back(p);
            }

            // 위쪽 껍질을 이어 쌓는다.
            const size_t lower = hull.size() + 1;
            for (size_t i = pts.size() - 1; i-- > 0;)
            {
                while (hull.size() >= lower && dropMiddle(hull[hull.size() - 2], hull[hull.size() - 1], pts[i]))
                {
                    hull.pop_back();
                }
                hull.push_back(pts[i]);
            }

            // 시작점이 중복되므로 마지막 점을 제거한다.
            hull.pop_back();

            // 정렬 순서상 양 끝에 놓인 꼭짓점까지 포함해, 이웃을 잇는 직선에서 tol 이내인 꼭짓점을 순환하며 제거한다.
            for (bool removed = true; removed && hull.size() > 3;)
            {
                removed = false;
                for (size_t i = 0; i < hull.size() && hull.size() > 3; ++i)
                {
                    const Point2& prev = hull[(i + hull.size() - 1) % hull.size()];
                    const Point2& next = hull[(i + 1) % hull.size()];
                    if (dropMiddle(prev, hull[i], next))
                    {
                        hull.erase(hull.begin() + static_cast<std::ptrdiff_t>(i));
                        removed = true;
                        break;
                    }
                }
            }

            // 껍질 점들을 정점 인덱스 목록으로 바꿔 돌려준다.
            std::vector<uint32_t> boundary;
            for (const Point2& p : hull)
            {
                boundary.push_back(p.index);
            }
            return boundary;
        }

        // 두 후보 면이 공유한 점 중 일직선이 아닌 3점 이상이 있으면 같은 면(허용치 안)으로 본다.
        bool SharesFacePlane(const std::vector<Vec3d>& v, const Face& a, const Face& b, double tol)
        {
            // 두 면이 공유한 정점 인덱스를 구하고 3개 미만이면 다른 면이다.
            std::vector<uint32_t> shared;
            std::set_intersection(a.Points.begin(), a.Points.end(), b.Points.begin(), b.Points.end(), std::back_inserter(shared));
            if (shared.size() < 3)
            {
                return false;
            }

            // 첫 공유점에서 뻗은 두 변이 일직선이 아닌 조합을 찾으면 같은 면이다.
            const Vec3d origin = v[shared[0]];
            for (size_t j = 1; j < shared.size(); ++j)
            {
                const Vec3d edge = v[shared[j]] - origin;
                const double edgeLen = std::sqrt(Dot(edge, edge));
                if (edgeLen <= tol)
                {
                    continue;
                }

                for (size_t k = j + 1; k < shared.size(); ++k)
                {
                    const Vec3d offset = Cross(edge, v[shared[k]] - origin);
                    if (std::sqrt(Dot(offset, offset)) > tol * edgeLen)
                    {
                        return true;
                    }
                }
            }

            // 모두 일직선이면 같은 면이 아니다.
            return false;
        }

        // 모든 점 삼중쌍으로 후보 평면을 만들어, 나머지 점이 모두 안쪽(tol 이내)인 평면만 면으로 모은다.
        // tol은 클리퍼와 같은 절대 거리 허용치이고, 각 면에는 경계 다각형·Newell 법선·지지 평면 상수항을 채운다.
        std::vector<Face> BuildFaces(const std::vector<Vec3d>& v, double tol)
        {
            // 정점이 4개 미만이면 면을 만들 수 없다.
            std::vector<Face> faces;
            if (v.size() < 4)
            {
                return faces;
            }

            // 모든 점 삼중쌍을 순회하며 후보 평면을 만든다.
            for (size_t i = 0; i < v.size(); ++i)
            {
                for (size_t j = i + 1; j < v.size(); ++j)
                {
                    for (size_t k = j + 1; k < v.size(); ++k)
                    {
                        // 세 점이 일직선에 가까우면 건너뛴다.
                        const double edgeLen = std::sqrt(Dot(v[j] - v[i], v[j] - v[i]));
                        Vec3d n = Cross(v[j] - v[i], v[k] - v[i]);
                        const double len = std::sqrt(Dot(n, n));
                        if (edgeLen == 0.0 || len <= tol * edgeLen)
                        {
                            continue;
                        }

                        // 법선을 정규화하고 상수항을 구한다.
                        n = n * (1.0 / len);
                        double w = Dot(n, v[i]);

                        // 모든 점의 부호 있는 거리 범위를 구한다.
                        double maxD = -std::numeric_limits<double>::max();
                        double minD = std::numeric_limits<double>::max();
                        for (const Vec3d& p : v)
                        {
                            const double d = Dot(n, p) - w;
                            maxD = std::max(maxD, d);
                            minD = std::min(minD, d);
                        }

                        // 양쪽에 점이 있으면 지지 평면이 아니고, 바깥쪽에만 있으면 법선을 뒤집는다.
                        if (maxD > tol)
                        {
                            if (minD < -tol)
                            {
                                continue;
                            }
                            n = n * -1.0;
                            w = -w;
                        }

                        // 평면 위(허용치 안)에 놓인 정점들을 면에 모은다.
                        Face face{ n, w, {}, {} };
                        for (size_t p = 0; p < v.size(); ++p)
                        {
                            if (std::fabs(Dot(n, v[p]) - w) <= tol)
                            {
                                face.Points.push_back(static_cast<uint32_t>(p));
                            }
                        }

                        // 같은 점 집합의 면이 아직 없을 때만 추가한다.
                        const bool known = std::any_of(faces.begin(), faces.end(), [&](const Face& f)
                        {
                            return f.Points == face.Points;
                        });
                        if (!known)
                        {
                            faces.push_back(face);
                        }
                    }
                }
            }

            // 허용치 안에서 같은 면인 후보들을 합집합으로 병합한다. 병합된 면의 내부점은 이후 경계 계산에서 빠진다.
            for (bool merged = true; merged;)
            {
                merged = false;
                for (size_t i = 0; i < faces.size() && !merged; ++i)
                {
                    for (size_t j = i + 1; j < faces.size() && !merged; ++j)
                    {
                        if (SharesFacePlane(v, faces[i], faces[j], tol))
                        {
                            std::vector<uint32_t> united;
                            std::set_union(faces[i].Points.begin(), faces[i].Points.end(),
                                faces[j].Points.begin(), faces[j].Points.end(), std::back_inserter(united));
                            faces[i].Points = united;
                            faces.erase(faces.begin() + static_cast<std::ptrdiff_t>(j));
                            merged = true;
                        }
                    }
                }
            }

            // 각 면의 경계 다각형을 구하고, 경계가 3점 미만이면 법선 계산을 건너뛴다.
            for (Face& face : faces)
            {
                face.Boundary = FaceBoundary(v, face, tol);
                if (face.Boundary.size() < 3)
                {
                    continue;
                }

                // Newell 법선: 경계 다각형 전체로 계산해 대각선 선택에 흔들리지 않게 한다.
                const Vec3d origin = v[face.Boundary[0]];
                Vec3d normal{ 0, 0, 0 };
                for (size_t i = 0; i < face.Boundary.size(); ++i)
                {
                    const Vec3d a = v[face.Boundary[i]] - origin;
                    const Vec3d b = v[face.Boundary[(i + 1) % face.Boundary.size()]] - origin;
                    normal = normal + Vec3d{ (a.y - b.y) * (a.z + b.z), (a.z - b.z) * (a.x + b.x), (a.x - b.x) * (a.y + b.y) };
                }
                face.Normal = normal * (1.0 / std::sqrt(Dot(normal, normal)));

                // 상수항은 모든 정점의 최대 Dot로 잡아 지지 평면이 되게 한다.
                face.W = -std::numeric_limits<double>::max();
                for (const Vec3d& p : v)
                {
                    face.W = std::max(face.W, Dot(face.Normal, p));
                }
            }

            // 만들어진 면 목록을 돌려준다.
            return faces;
        }
    }

    // 다각형을 평면 안쪽으로 자른다. 정점을 -1/0/+1로 분류하고 부호가 엄격히 반대인 변에서만 교점을 만든다.
    std::vector<Vec3d> ClipPolygonInside(const std::vector<Vec3d>& poly, const Plane& plane, double eps)
    {
        // 결과 다각형과 정점 수를 준비한다.
        std::vector<Vec3d> out;
        const size_t count = poly.size();

        // 각 변의 양 끝점을 평면 기준 -1/0/+1로 분류한다.
        for (size_t i = 0; i < count; ++i)
        {
            const Vec3d a = poly[i];
            const Vec3d b = poly[(i + 1) % count];
            const double da = plane.PlaneDot(a);
            const double db = plane.PlaneDot(b);
            const int sa = da > eps ? 1 : (da < -eps ? -1 : 0);
            const int sb = db > eps ? 1 : (db < -eps ? -1 : 0);

            // 시작점이 안쪽이거나 평면 위면 유지한다.
            if (sa <= 0)
            {
                out.push_back(a);
            }

            // 부호가 엄격히 반대인 변에서만 교점을 추가한다.
            if (sa * sb == -1)
            {
                out.push_back(a + (b - a) * (da / (da - db)));
            }
        }

        // 잘린 다각형을 돌려준다.
        return out;
    }

    // 정점·인덱스·경계 상자를 모두 비운다.
    void ConvexElem::Reset()
    {
        // 정점과 인덱스를 비운다.
        VertexData.clear();
        IndexData.clear();

        // 경계 상자를 초기 상태로 되돌린다.
        ElemBox = Box();
    }

    // 현재 정점들의 최소·최대 좌표로 ElemBox를 다시 계산하고 인덱스를 재구성한다(Unreal UpdateElemBox와 같은 역할).
    void ConvexElem::UpdateElemBox(double faceTol)
    {
        // 이전 인덱스를 지우고 경계 상자를 다시 계산한다.
        IndexData.clear();
        UpdateBoundsOnly();

        // 정점이 없으면 인덱스 재구성 없이 끝낸다.
        if (VertexData.empty())
        {
            return;
        }

        // 인덱스를 재구성한다.
        ComputeHullIndices(faceTol);
    }

    // 정점의 최소·최대 좌표로 ElemBox만 다시 계산한다(인덱스·정점은 건드리지 않는다).
    void ConvexElem::UpdateBoundsOnly()
    {
        // 이전 상자를 지운다. 정점이 없으면 빈 상자로 끝낸다.
        ElemBox = Box();
        if (VertexData.empty())
        {
            return;
        }

        // 모든 정점의 최소·최대 좌표를 구한다.
        ElemBox.Min = ElemBox.Max = VertexData[0];
        for (const Vec3d& v : VertexData)
        {
            ElemBox.Min = { std::min(ElemBox.Min.x, v.x), std::min(ElemBox.Min.y, v.y), std::min(ElemBox.Min.z, v.z) };
            ElemBox.Max = { std::max(ElemBox.Max.x, v.x), std::max(ElemBox.Max.y, v.y), std::max(ElemBox.Max.z, v.z) };
        }
        ElemBox.IsValid = true;
    }

    // 정점에 3x4 아핀(m[i][0..2]=선형 행, m[i][3]=이동)을 적용한다. 인덱스는 유지하고 행렬식이 음수면 삼각형 감김을 뒤집는다.
    void ConvexElem::ApplyAffine(const double m[3][4])
    {
        // 모든 정점을 변환한다.
        for (Vec3d& v : VertexData)
        {
            v = { m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3],
                  m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3],
                  m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3] };
        }

        // 선형부 행렬식이 음수(거울 변환)면 삼각형 감김을 뒤집어 바깥 방향을 유지한다.
        const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1])
            - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
            + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
        if (det < 0.0)
        {
            for (size_t t = 0; t + 2 < IndexData.size(); t += 3)
            {
                std::swap(IndexData[t + 1], IndexData[t + 2]);
            }
        }

        // 경계 상자만 다시 계산한다.
        UpdateBoundsOnly();
    }

    // VertexData에서 faceTol 이내 중복 정점을 정리한 뒤 볼록 껍질 면 경계 다각형을 fan으로 삼각화하고, 삼각형이 쓰지 않는 정점(면 내부·변 위 점)은 제거한다.
    // 성공하면 VertexData는 hull 모서리 정점만 남고, 닫힌 곡면이 아니면 IndexData를 비우고 false를 돌려준다.
    bool ConvexElem::ComputeHullIndices(double faceTol)
    {
        // 이전 인덱스를 비운다.
        IndexData.clear();

        // faceTol 이내로 겹친 정점은 같은 점으로 보고 먼저 나온 것만 남긴다.
        std::vector<Vec3d> distinct;
        for (const Vec3d& v : VertexData)
        {
            const bool duplicate = std::any_of(distinct.begin(), distinct.end(), [&](const Vec3d& d)
            {
                return Dot(v - d, v - d) <= faceTol * faceTol;
            });
            if (!duplicate)
            {
                distinct.push_back(v);
            }
        }
        VertexData = distinct;

        // 볼록 껍질 면을 만들고 4개 미만이면 실패한다.
        const std::vector<Face> faces = BuildFaces(VertexData, faceTol);
        if (faces.size() < 4)
        {
            return false;
        }

        // 각 면의 경계 다각형을 fan으로 삼각화한다.
        for (const Face& face : faces)
        {
            for (size_t k = 1; k + 1 < face.Boundary.size(); ++k)
            {
                IndexData.push_back(face.Boundary[0]);
                IndexData.push_back(face.Boundary[k]);
                IndexData.push_back(face.Boundary[k + 1]);
            }
        }

        // 모든 방향 변이 정확히 한 번씩 나오고 반대 방향 변도 짝으로 있어야 닫힌 곡면이다.
        std::map<std::pair<uint32_t, uint32_t>, int> edges;
        for (size_t t = 0; t + 2 < IndexData.size(); t += 3)
        {
            for (size_t e = 0; e < 3; ++e)
            {
                ++edges[{ IndexData[t + e], IndexData[t + (e + 1) % 3] }];
            }
        }
        for (const auto& entry : edges)
        {
            const auto reverse = edges.find({ entry.first.second, entry.first.first });
            if (entry.second != 1 || reverse == edges.end() || reverse->second != 1)
            {
                IndexData.clear();
                return false;
            }
        }

        // 쓰이지 않는 정점을 제거하고 인덱스를 새 번호로 바꾼다.
        std::vector<uint32_t> remap(VertexData.size(), UINT32_MAX);
        std::vector<Vec3d> kept;
        for (uint32_t& index : IndexData)
        {
            if (remap[index] == UINT32_MAX)
            {
                remap[index] = static_cast<uint32_t>(kept.size());
                kept.push_back(VertexData[index]);
            }
            index = remap[index];
        }
        VertexData = kept;

        // 닫힌 hull 재구성에 성공했음을 알린다.
        return true;
    }

    // 평면마다 큰 사각형을 나머지 평면으로 잘라 남은 다각형 정점을 snap·병합 없이 모은다(거의 같은 점만 제거).
    void CollectPlanePolygonVertices(const std::vector<Plane>& planes, const HullTolerances& tol, std::vector<Vec3d>& out)
    {
        // 이전 결과를 비운다.
        out.clear();

        // 평면마다 잘린 다각형의 정점을 EpsDistance 안에서 중복 없이 추가한다.
        for (size_t i = 0; i < planes.size(); ++i)
        {
            for (const Vec3d& p : ClipPlanePolygon(planes, i, tol))
            {
                AddVertexIfNotPresent(out, p, tol.EpsDistance);
            }
        }
    }

    // Unreal FKConvexElem::HullFromPlanes와 같은 순서로 바깥 법선 평면들의 안쪽 교집합 hull을 만든다.
    bool ConvexElem::HullFromPlanes(
        const std::vector<Plane>& planes,
        const std::vector<Vec3d>& snapVerts,
        double snapDistSq,
        const HullTolerances& tol)
    {
        // 이전 결과를 모두 비운다.
        Reset();

        // 평면마다 큰 사각형을 만들어 나머지 평면으로 자르고, 남은 정점을 snap·병합해 모은다.
        double totalArea = 0.0;
        for (size_t i = 0; i < planes.size(); ++i)
        {
            // 평면 위의 큰 사각형을 나머지 평면으로 자른다.
            const std::vector<Vec3d> poly = ClipPlanePolygon(planes, i, tol);

            // 면이 사라졌으면 다음 평면으로 넘어간다.
            if (poly.size() < 3)
            {
                continue;
            }

            // 면적을 누적하고 정점을 snap·병합해 추가한다.
            totalArea += PolygonArea(poly);
            for (const Vec3d& p : poly)
            {
                AddVertexIfNotPresent(VertexData, SnapVertex(p, snapVerts, snapDistSq), tol.MinVertDistance);
            }
        }

        // 총 면적이나 정점 수가 부족하면 실패한다.
        if (totalArea < tol.MinArea || VertexData.size() < 4)
        {
            return false;
        }

        // 모든 정점이 한 직선 위인지 확인한다.
        Vec3d dir1 = VertexData[1] - VertexData[0];
        dir1 = dir1 * (1.0 / std::sqrt(Dot(dir1, dir1)));
        Vec3d dir2{ 0, 0, 0 };
        bool found = false;
        for (size_t i = 2; i < VertexData.size() && !found; ++i)
        {
            dir2 = VertexData[i] - VertexData[0];
            dir2 = dir2 * (1.0 / std::sqrt(Dot(dir2, dir2)));
            found = Dot(dir1, dir2) < 1.0 - tol.EpsAngle;
        }
        if (!found)
        {
            return false;
        }

        // 모든 정점이 한 평면 위인지 확인한다.
        Vec3d normal = Cross(dir1, dir2);
        normal = normal * (1.0 / std::sqrt(Dot(normal, normal)));
        const Plane flat{ normal, Dot(normal, VertexData[0]) };
        found = false;
        for (size_t i = 2; i < VertexData.size() && !found; ++i)
        {
            found = std::fabs(flat.PlaneDot(VertexData[i])) > tol.EpsDistance;
        }
        if (!found)
        {
            return false;
        }

        // 경계 상자와 인덱스를 만들고, 인덱스가 비면 실패로 되돌린다.
        UpdateElemBox(tol.EpsDistance);
        if (IndexData.empty())
        {
            Reset();
            return false;
        }

        // hull 생성에 성공했음을 알린다.
        return true;
    }

    // 재구성된 면마다 Newell 법선의 지지 평면 하나를 돌려준다. 모든 정점이 PlaneDot <= 0이다(Chaos 비의존, 우리 구현).
    void ConvexElem::GetPlanes(std::vector<Plane>& out, double faceTol) const
    {
        // 면마다 법선과 상수항을 평면으로 추가한다.
        for (const Face& face : BuildFaces(VertexData, faceTol))
        {
            out.push_back({ face.Normal, face.W });
        }
    }

    // 정점 평균을 기준점으로 삼아 사면체 부피를 더한다. 큰 좌표에서도 정밀도가 유지되며 바깥 감김이면 양수다.
    double ConvexElem::GetVolume() const
    {
        // 정점이 없으면 부피는 0이다.
        if (VertexData.empty())
        {
            return 0.0;
        }

        // 정점 평균을 기준점으로 잡는다.
        Vec3d ref{ 0, 0, 0 };
        for (const Vec3d& v : VertexData)
        {
            ref = ref + v * (1.0 / static_cast<double>(VertexData.size()));
        }

        // 삼각형마다 기준점과 이루는 사면체 부피를 더한다.
        double sum = 0.0;
        for (size_t t = 0; t + 2 < IndexData.size(); t += 3)
        {
            const Vec3d a = VertexData[IndexData[t]] - ref;
            sum += Dot(a, Cross(VertexData[IndexData[t + 1]] - ref, VertexData[IndexData[t + 2]] - ref));
        }

        // 사면체 부피 합을 6으로 나눠 돌려준다.
        return sum / 6.0;
    }
}
