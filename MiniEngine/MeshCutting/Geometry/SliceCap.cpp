// 이 파일은 잘린 조각의 절단 경계에서 루프를 뽑아 검증하는 CutLoops와, 그 루프를 캡 면으로 채우는 CapBuilder를 구현한다.
#include "../pch.h"
#include "Slice.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace MeshCutting
{
    namespace
    {
        struct Point2
        {
            double x, y;
        };

        double Cross(const Point2& o, const Point2& a, const Point2& b)
        {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        }

        double PointSegmentDistance(const Point2& p, const Point2& a, const Point2& b)
        {
            const double dx = b.x - a.x, dy = b.y - a.y;
            const double lengthSquared = dx * dx + dy * dy;
            double t = 0.0;
            if (lengthSquared > 0.0)
                t = std::max(0.0, std::min(1.0, ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSquared));
            const double ex = p.x - (a.x + t * dx), ey = p.y - (a.y + t * dy);
            return std::sqrt(ex * ex + ey * ey);
        }

        // 0 when the segments cross, otherwise the smallest end point distance.
        double SegmentDistance(const Point2& a, const Point2& b, const Point2& c, const Point2& d)
        {
            const double o1 = Cross(a, b, c), o2 = Cross(a, b, d), o3 = Cross(c, d, a), o4 = Cross(c, d, b);
            if (((o1 > 0.0 && o2 < 0.0) || (o1 < 0.0 && o2 > 0.0)) && ((o3 > 0.0 && o4 < 0.0) || (o3 < 0.0 && o4 > 0.0)))
                return 0.0;
            return std::min(std::min(PointSegmentDistance(a, c, d), PointSegmentDistance(b, c, d)),
                std::min(PointSegmentDistance(c, a, b), PointSegmentDistance(d, a, b)));
        }

        bool PointInPolygon(const Point2& p, const std::vector<Point2>& polygon)
        {
            bool inside = false;
            for (size_t i = 0, j = polygon.size() - 1; i < polygon.size(); j = i++)
            {
                if ((polygon[i].y > p.y) != (polygon[j].y > p.y)
                    && p.x < (polygon[j].x - polygon[i].x) * (p.y - polygon[i].y) / (polygon[j].y - polygon[i].y) + polygon[i].x)
                    inside = !inside;
            }
            return inside;
        }
    }

    SliceStatus ExtractCutLoops(const MeshGeometry& piece, const std::vector<uint8_t>& onPlane,
        const SlicePlane& plane, bool positiveSide, float epsilonScale,
        std::vector<SliceLoop>& loops, std::string* detail, std::vector<std::vector<uint32_t>>* uncapped)
    {
        loops.clear();
        if (uncapped)
            uncapped->clear();
        if (detail)
            detail->clear();
        SlicePlaneBasis basis;
        if (!MakePlaneBasis(plane, basis))
            return Fail(detail, SliceStatus::InvalidInput, "plane is invalid");
        if (onPlane.size() != piece.vertices.size())
            return Fail(detail, SliceStatus::InvalidInput, "onPlane does not match the vertex count");
        const double epsilon = ComputePieceRadius(piece) * double(epsilonScale);

        // Directed cut edges from the faces: each boundary edge belongs to exactly one face.
        const size_t nodeCount = piece.vertices.size();
        std::vector<uint32_t> outCount(nodeCount, 0), inCount(nodeCount, 0), next(nodeCount, kInvalidIndex);
        std::vector<std::vector<uint32_t>> cutEdges;      // (from, to): 루프를 만들 수 없을 때 cap 없는 경계로 그대로 넘긴다
        for (const MeshFace& face : piece.faces)
        {
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t from = piece.corners[face.corner[k]].vertex;
                const uint32_t to = piece.corners[face.corner[(k + 1) % 3]].vertex;
                const auto tag = piece.edgeTags.find(MakeEdgeKey(from, to));
                if (tag == piece.edgeTags.end() || tag->second != MeshEdgeTag::Cut || !onPlane[from] || !onPlane[to])
                    continue;
                ++outCount[from];
                ++inCount[to];
                next[from] = to;
                cutEdges.push_back({ from, to });
            }
        }

        // What else the boundary touches at a vertex, in key order so the message is stable.
        std::vector<int> blocker(nodeCount, -1);
        std::vector<uint8_t> onHole(nodeCount, 0);      // OpenSurface 에지(원본의 구멍)에 닿은 정점
        std::vector<uint64_t> keys;
        keys.reserve(piece.edgeTags.size());
        for (const auto& entry : piece.edgeTags)
            keys.push_back(entry.first);
        std::sort(keys.begin(), keys.end());
        for (uint64_t key : keys)
        {
            const MeshEdgeTag kind = piece.edgeTags.at(key);
            if (kind == MeshEdgeTag::CapSeam)
                continue;
            const uint32_t a = EdgeKeyLow(key), b = EdgeKeyHigh(key);
            if (a >= nodeCount || b >= nodeCount)
                continue;
            if (kind == MeshEdgeTag::Cut && onPlane[a] && onPlane[b])
                continue;       // a cut edge of this plane, not a blocker
            if (kind == MeshEdgeTag::OpenSurface)
                onHole[a] = onHole[b] = 1;
            if (blocker[a] < 0)
                blocker[a] = int(kind);
            if (blocker[b] < 0)
                blocker[b] = int(kind);
        }

        // Degree check: open ends caused by another boundary are OpenLoop, the rest NonManifoldLoop.
        uint32_t openNode = kInvalidIndex, badNode = kInvalidIndex;
        for (uint32_t n = 0; n < nodeCount; ++n)
        {
            const uint32_t out = outCount[n], in = inCount[n];
            if (out == 0 && in == 0)
                continue;
            if (out == 1 && in == 1)
                continue;
            if (out + in == 1 && onHole[n] && uncapped)
                continue;       // 원본의 구멍에서 끝나는 열린 사슬의 끝 (아래에서 사슬로 뽑는다)
            if (out + in == 1 && blocker[n] >= 0)
            {
                if (openNode == kInvalidIndex)
                    openNode = n;
            }
            else if (badNode == kInvalidIndex)
                badNode = n;
        }
        // 스켈레탈 패치 경계(Replacement/UnweldedSeam)에서 끝나는 절단은 패치를 잘못 잡았다는 뜻이라 어느 모드든 거절한다
        if (openNode != kInvalidIndex && (uncapped || badNode == kInvalidIndex))
            return Fail(detail, SliceStatus::OpenLoop, "cut loop ends at vertex " + Num(openNode) + " on a "
                + EdgeTagName(MeshEdgeTag(blocker[openNode])) + " boundary");
        if (badNode != kInvalidIndex)
        {
            if (!uncapped)
                return Fail(detail, SliceStatus::NonManifoldLoop, "cut vertex " + Num(badNode) + " has " + Num(outCount[badNode])
                    + " outgoing and " + Num(inCount[badNode]) + " incoming cut edges");
            // Unreal은 이런 정점도 거리로 이어 붙이고 닫히지 않으면 cap을 버린다. 여기서는 루프를 만들지 않고 모든 절단 에지를 cap 없이 둔다
            *uncapped = std::move(cutEdges);
            return SliceStatus::Ok;
        }

        // Open chains first: each starts at a hole end with no incoming cut edge and follows next until
        // the other hole end. The degree check above already allowed only hole ends to have degree one.
        std::vector<uint8_t> visited(nodeCount, 0);
        for (uint32_t start = 0; uncapped && start < nodeCount; ++start)
        {
            if (outCount[start] != 1 || inCount[start] != 0)
                continue;
            std::vector<uint32_t> chain;
            uint32_t vertex = start;
            while (vertex != kInvalidIndex && !visited[vertex])
            {
                visited[vertex] = 1;
                chain.push_back(vertex);
                vertex = outCount[vertex] == 1 ? next[vertex] : kInvalidIndex;
            }
            if (vertex != kInvalidIndex)
                return Fail(detail, SliceStatus::NonManifoldLoop, "open cut chain from vertex " + Num(start) + " runs into itself");
            uncapped->push_back(std::move(chain));
        }

        // Unreal처럼 채울 수 없는 루프는 절단을 막지 않고 cap만 뺀다 (uncapped가 있을 때). 닫힌 루프는 첫 정점을 끝에 한 번 더 넣어
        // 닫는 에지까지 연속한 쌍으로 표현한다. uncapped가 없으면 예전처럼 실패를 돌려준다.
        const auto setAside = [&](std::vector<uint32_t> vertices)
        {
            if (!vertices.empty())
                vertices.push_back(vertices.front());
            uncapped->push_back(std::move(vertices));
        };

        // Every remaining cut vertex has one way in and one way out, so the cut edges are cycles.
        for (uint32_t start = 0; start < nodeCount; ++start)
        {
            if (outCount[start] != 1 || visited[start])
                continue;
            SliceLoop loop;
            uint32_t vertex = start;
            do
            {
                visited[vertex] = 1;
                loop.vertices.push_back(vertex);
                vertex = next[vertex];
            } while (vertex != start);
            if (loop.vertices.size() < 3)
            {
                if (!uncapped)
                    return Fail(detail, SliceStatus::InvalidLoop, "loop through vertex " + Num(start) + " has only "
                        + Num(loop.vertices.size()) + " vertices");
                setAside(std::move(loop.vertices));
                continue;
            }
            loops.push_back(std::move(loop));
        }
        if (loops.empty())
            return SliceStatus::Ok;

        // Project into the plane.
        std::vector<std::vector<Point2>> points(loops.size());
        for (size_t l = 0; l < loops.size(); ++l)
        {
            for (uint32_t n : loops[l].vertices)
            {
                const DirectX::XMFLOAT3& p = piece.vertices[n].position;
                const double d[3] = { double(p.x) - basis.origin[0], double(p.y) - basis.origin[1], double(p.z) - basis.origin[2] };
                points[l].push_back({ d[0] * basis.u[0] + d[1] * basis.u[1] + d[2] * basis.u[2],
                    d[0] * basis.v[0] + d[1] * basis.v[1] + d[2] * basis.v[2] });
            }
        }

        // 검사에 걸린 루프는 엄격 모드(uncapped 없음)면 실패, 아니면 keep을 끈다 (cap 없이 남는다).
        std::vector<uint8_t> keep(loops.size(), 1);
        SliceStatus strictStatus = SliceStatus::Ok;
        const auto reject = [&](size_t l, const std::string& why)
        {
            if (!uncapped && strictStatus == SliceStatus::Ok)
                strictStatus = Fail(detail, SliceStatus::InvalidLoop, why);
            keep[l] = 0;
        };

        // Segments of one loop may only meet at their shared vertex; loops may not meet at all.
        for (size_t a = 0; a < loops.size() && strictStatus == SliceStatus::Ok; ++a)
        {
            const size_t na = points[a].size();
            for (size_t i = 0; i < na; ++i)
            {
                const Point2 &a0 = points[a][i], &a1 = points[a][(i + 1) % na];
                for (size_t j = i + 2; j < na; ++j)
                {
                    if (i == 0 && j == na - 1)
                        continue;       // adjacent through the closing segment
                    if (SegmentDistance(a0, a1, points[a][j], points[a][(j + 1) % na]) <= epsilon)
                        reject(a, "loop " + Num(a) + " crosses or touches itself between vertices "
                            + Num(loops[a].vertices[i]) + " and " + Num(loops[a].vertices[j]));
                }
                for (size_t b = a + 1; b < loops.size(); ++b)
                {
                    const size_t nb = points[b].size();
                    for (size_t j = 0; j < nb; ++j)
                        if (SegmentDistance(a0, a1, points[b][j], points[b][(j + 1) % nb]) <= epsilon)
                        {
                            reject(a, "loops " + Num(a) + " and " + Num(b) + " cross or touch near vertices "
                                + Num(loops[a].vertices[i]) + " and " + Num(loops[b].vertices[j]));
                            keep[b] = 0;
                        }
                }
            }
        }
        if (strictStatus != SliceStatus::Ok)
            return strictStatus;

        // Signed areas, containment and winding.
        for (size_t l = 0; l < loops.size(); ++l)
        {
            double area = 0.0;
            const size_t n = points[l].size();
            for (size_t i = 0; i < n; ++i)
            {
                const Point2 &p = points[l][i], &q = points[l][(i + 1) % n];
                area += p.x * q.y - q.x * p.y;
            }
            loops[l].signedArea = 0.5 * area;
            if (std::fabs(loops[l].signedArea) <= epsilon * epsilon)
                reject(l, "loop " + Num(l) + " has no area");
        }
        if (strictStatus != SliceStatus::Ok)
            return strictStatus;

        // 포함 관계는 남은 루프끼리만 본다.
        for (size_t l = 0; l < loops.size(); ++l)
        {
            if (!keep[l])
                continue;
            uint32_t depth = 0;
            uint32_t parent = kInvalidIndex;
            for (size_t m = 0; m < loops.size(); ++m)
            {
                if (m == l || !keep[m] || std::fabs(loops[m].signedArea) <= std::fabs(loops[l].signedArea))
                    continue;
                if (!PointInPolygon(points[l][0], points[m]))
                    continue;
                ++depth;
                if (parent == kInvalidIndex || std::fabs(loops[m].signedArea) < std::fabs(loops[parent].signedArea))
                    parent = uint32_t(m);
            }
            loops[l].depth = depth;
            loops[l].parent = parent;
        }
        for (size_t l = 0; l < loops.size() && strictStatus == SliceStatus::Ok; ++l)
            if (keep[l] && loops[l].depth > 1)
                reject(l, "loop " + Num(l) + " is nested " + Num(loops[l].depth)
                    + " deep; islands inside holes are not supported");
        if (strictStatus != SliceStatus::Ok)
            return strictStatus;
        const double outerSign = positiveSide ? 1.0 : -1.0;
        for (size_t l = 0; l < loops.size() && strictStatus == SliceStatus::Ok; ++l)
        {
            if (!keep[l])
                continue;
            const double expected = loops[l].depth == 0 ? outerSign : -outerSign;
            if (loops[l].signedArea * expected <= 0.0)
                reject(l, "loop " + Num(l) + " winds the wrong way for "
                    + (loops[l].depth == 0 ? "an outer boundary" : "a hole"));
        }
        if (strictStatus != SliceStatus::Ok)
            return strictStatus;

        // 바깥 루프가 빠지면 그 구멍도 뺀다. 구멍만 빠지면 바깥 루프가 구멍 자리까지 채운다 (Unreal과 같은 결과).
        for (size_t l = 0; l < loops.size(); ++l)
            if (keep[l] && loops[l].depth == 1 && (loops[l].parent == kInvalidIndex || !keep[loops[l].parent]))
                keep[l] = 0;

        // 남은 루프를 앞으로 모으고 구멍의 parent를 새 번호로 바꾼다. 뺀 루프는 cap 없는 경계로 넘긴다.
        std::vector<uint32_t> newIndex(loops.size(), kInvalidIndex);
        std::vector<SliceLoop> kept;
        for (size_t l = 0; l < loops.size(); ++l)
        {
            if (keep[l])
            {
                newIndex[l] = uint32_t(kept.size());
                kept.push_back(std::move(loops[l]));
            }
            else
                setAside(std::move(loops[l].vertices));
        }
        for (SliceLoop& loop : kept)
            if (loop.depth == 1)
                loop.parent = newIndex[loop.parent];
        loops = std::move(kept);
        return SliceStatus::Ok;
    }

    namespace
    {
        // One vertex of a ring being triangulated. Bridge edges repeat a vertex, so a vertex id can
        // appear twice in the combined ring.
        struct RingVertex
        {
            uint32_t vertex;
            Point2 p;
        };

        double SignedArea(const std::vector<RingVertex>& ring)
        {
            double area = 0.0;
            for (size_t i = 0; i < ring.size(); ++i)
            {
                const Point2 &a = ring[i].p, &b = ring[(i + 1) % ring.size()].p;
                area += a.x * b.y - b.x * a.y;
            }
            return 0.5 * area;
        }

        bool PointInPolygon(const Point2& p, const std::vector<RingVertex>& ring)
        {
            bool inside = false;
            for (size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++)
            {
                const Point2 &a = ring[i].p, &b = ring[j].p;
                if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x)
                    inside = !inside;
            }
            return inside;
        }

        // Joins `hole` (clockwise) to `ring` (counter-clockwise) with a bridge to the nearest
        // vertex that sees it. `walls` are all original rings; the bridge may not cross any
        // edge of them and must run through the material (inside the outer ring, outside every hole).
        bool BridgeHole(std::vector<RingVertex>& ring, const std::vector<RingVertex>& hole,
            const std::vector<std::vector<RingVertex>>& walls, size_t outerWall, double epsilon)
        {
            // Start the bridge at the hole vertex farthest to the right.
            size_t m = 0;
            for (size_t i = 1; i < hole.size(); ++i)
                if (hole[i].p.x > hole[m].p.x)
                    m = i;
            const RingVertex& from = hole[m];

            std::vector<size_t> order(ring.size());
            for (size_t i = 0; i < order.size(); ++i)
                order[i] = i;
            const auto distanceSquared = [&](size_t i)
            {
                const double dx = ring[i].p.x - from.p.x, dy = ring[i].p.y - from.p.y;
                return dx * dx + dy * dy;
            };
            std::sort(order.begin(), order.end(), [&](size_t a, size_t b)
            {
                const double da = distanceSquared(a), db = distanceSquared(b);
                return da != db ? da < db : a < b;
            });

            for (size_t candidate : order)
            {
                const RingVertex& to = ring[candidate];
                bool blocked = false;
                for (const std::vector<RingVertex>& wall : walls)
                {
                    for (size_t i = 0; i < wall.size() && !blocked; ++i)
                    {
                        const RingVertex &a = wall[i], &b = wall[(i + 1) % wall.size()];
                        if (a.vertex == from.vertex || b.vertex == from.vertex || a.vertex == to.vertex || b.vertex == to.vertex)
                            continue;
                        blocked = SegmentDistance(from.p, to.p, a.p, b.p) <= epsilon;
                    }
                    if (blocked)
                        break;
                }
                if (blocked)
                    continue;
                // The bridge must run through the material: inside the outer ring, outside every hole.
                const Point2 mid = { (from.p.x + to.p.x) * 0.5, (from.p.y + to.p.y) * 0.5 };
                bool material = PointInPolygon(mid, walls[outerWall]);
                for (size_t w = 0; w < walls.size() && material; ++w)
                    if (w != outerWall && PointInPolygon(mid, walls[w]))
                        material = false;
                if (!material)
                    continue;

                // ... to, from, hole after from ..., from, to, rest of the ring ...
                std::vector<RingVertex> joined(ring.begin(), ring.begin() + candidate + 1);
                for (size_t k = 0; k <= hole.size(); ++k)
                    joined.push_back(hole[(m + k) % hole.size()]);
                joined.push_back(to);
                joined.insert(joined.end(), ring.begin() + candidate + 1, ring.end());
                ring.swap(joined);
                return true;
            }
            return false;
        }

        // Ear clipping over a counter-clockwise ring that may repeat vertices (bridges). Only
        // strictly convex tips with enough area are clipped, so straight rim vertices stay in the ring
        // and every ring edge ends up on exactly one triangle.
        bool EarClip(const std::vector<RingVertex>& ring, double epsilon, std::vector<uint32_t>& triangles, std::string& why)
        {
            const size_t n = ring.size();
            std::vector<size_t> prev(n), next(n);
            for (size_t i = 0; i < n; ++i)
            {
                prev[i] = (i + n - 1) % n;
                next[i] = (i + 1) % n;
            }
            const double minCross = 4.0 * epsilon * epsilon;        // area > 2 epsilon^2, above the validator's threshold
            size_t remaining = n, tip = 0;

            while (remaining > 3)
            {
                bool clipped = false;
                size_t i = tip;
                for (size_t tried = 0; tried < remaining; ++tried, i = next[i])
                {
                    const size_t p = prev[i], q = next[i];
                    if (Cross(ring[p].p, ring[i].p, ring[q].p) <= minCross)
                        continue;
                    bool blocked = false;
                    for (size_t w = next[q]; w != p && !blocked; w = next[w])
                    {
                        if (ring[w].vertex == ring[p].vertex || ring[w].vertex == ring[i].vertex || ring[w].vertex == ring[q].vertex)
                            continue;
                        blocked = Cross(ring[p].p, ring[i].p, ring[w].p) >= -minCross
                            && Cross(ring[i].p, ring[q].p, ring[w].p) >= -minCross
                            && Cross(ring[q].p, ring[p].p, ring[w].p) >= -minCross;
                    }
                    if (blocked)
                        continue;
                    triangles.insert(triangles.end(), { ring[p].vertex, ring[i].vertex, ring[q].vertex });
                    next[p] = q;
                    prev[q] = p;
                    tip = p;
                    --remaining;
                    clipped = true;
                    break;
                }
                if (!clipped)
                {
                    why = "no ear found with " + Num(remaining) + " ring vertices left";
                    return false;
                }
            }
            const size_t a = tip, b = next[a], c = next[b];
            if (Cross(ring[a].p, ring[b].p, ring[c].p) <= minCross)
            {
                why = "the last triangle has no area";
                return false;
            }
            triangles.insert(triangles.end(), { ring[a].vertex, ring[b].vertex, ring[c].vertex });
            return true;
        }

        SliceStatus Fail(std::string* detail, const std::string& message)
        {
            if (detail)
                *detail = message;
            return SliceStatus::CapTriangulationFailed;
        }
    }

    SliceStatus BuildCap(MeshGeometry& piece, const SlicePlane& plane, bool positiveSide, uint32_t cutId,
        const std::vector<SliceLoop>& loops, float epsilonScale, float uvScale, std::string* detail)
    {
        if (detail)
            detail->clear();
        if (loops.empty())
            return SliceStatus::Ok;
        SlicePlaneBasis basis;
        if (!MakePlaneBasis(plane, basis))
            return Fail(detail, "plane is invalid");
        const double epsilon = ComputePieceRadius(piece) * double(epsilonScale);

        // Loops in plane coordinates, as rings.
        std::vector<std::vector<RingVertex>> rings(loops.size());
        for (size_t l = 0; l < loops.size(); ++l)
            for (uint32_t vertex : loops[l].vertices)
            {
                if (vertex >= piece.vertices.size())
                    return Fail(detail, "loop vertex " + Num(vertex) + " is out of range");
                const DirectX::XMFLOAT3& pos = piece.vertices[vertex].position;
                const double d[3] = { double(pos.x) - basis.origin[0], double(pos.y) - basis.origin[1], double(pos.z) - basis.origin[2] };
                rings[l].push_back({ vertex, { d[0] * basis.u[0] + d[1] * basis.u[1] + d[2] * basis.u[2],
                    d[0] * basis.v[0] + d[1] * basis.v[1] + d[2] * basis.v[2] } });
            }

        // The triangulator wants outer rings counter-clockwise and holes clockwise.
        double expectedArea = 0.0;
        std::vector<std::vector<RingVertex>> oriented = rings;
        for (size_t l = 0; l < loops.size(); ++l)
        {
            const double area = SignedArea(oriented[l]);
            const bool outer = loops[l].depth == 0;
            if ((outer && area < 0.0) || (!outer && area > 0.0))
                std::reverse(oriented[l].begin(), oriented[l].end());
            expectedArea += outer ? std::fabs(area) : -std::fabs(area);
        }

        std::vector<uint32_t> triangles;        // vertex triples, counter-clockwise in (u, v)
        for (size_t l = 0; l < loops.size(); ++l)
        {
            if (loops[l].depth != 0)
                continue;
            std::vector<size_t> holes;
            for (size_t h = 0; h < loops.size(); ++h)
                if (loops[h].depth == 1 && loops[h].parent == l)
                    holes.push_back(h);
            // Rightmost holes first, so a bridge from a later hole cannot cross an earlier one.
            std::sort(holes.begin(), holes.end(), [&](size_t a, size_t b)
            {
                double ma = -1e300, mb = -1e300;
                for (const RingVertex& v : oriented[a]) ma = std::max(ma, v.p.x);
                for (const RingVertex& v : oriented[b]) mb = std::max(mb, v.p.x);
                return ma != mb ? ma > mb : a < b;
            });

            std::vector<std::vector<RingVertex>> walls;
            walls.push_back(oriented[l]);
            for (size_t h : holes)
                walls.push_back(oriented[h]);
            std::vector<RingVertex> ring = oriented[l];
            for (size_t h : holes)
                if (!BridgeHole(ring, oriented[h], walls, 0, epsilon))
                    return Fail(detail, "hole loop " + Num(h) + " cannot be joined to loop " + Num(l));
            std::string why;
            if (!EarClip(ring, epsilon, triangles, why))
                return Fail(detail, "loop " + Num(l) + ": " + why);
        }

        // Append the cap: one corner per rim vertex, outward normal, planar UVs. A copy of the piece
        // is edited first so a failed check leaves the caller's piece untouched.
        MeshGeometry capped = piece;
        const double sign = positiveSide ? -1.0 : 1.0;      // cap normal = -n for the positive piece
        const DirectX::XMFLOAT3 normal = { float(sign * basis.n[0]), float(sign * basis.n[1]), float(sign * basis.n[2]) };
        std::unordered_map<uint32_t, uint32_t> cornerOf;
        const auto cornerFor = [&](uint32_t vertex)
        {
            const auto found = cornerOf.find(vertex);
            if (found != cornerOf.end())
                return found->second;
            const DirectX::XMFLOAT3& pos = capped.vertices[vertex].position;
            const double d[3] = { double(pos.x) - basis.origin[0], double(pos.y) - basis.origin[1], double(pos.z) - basis.origin[2] };
            MeshCorner corner;
            corner.vertex = vertex;
            corner.normal = normal;
            corner.uv = { float((d[0] * basis.u[0] + d[1] * basis.u[1] + d[2] * basis.u[2]) * double(uvScale)),
                float((d[0] * basis.v[0] + d[1] * basis.v[1] + d[2] * basis.v[2]) * double(uvScale)) };
            corner.color = { 0.9f, 0.9f, 0.9f, 1.0f };
            const uint32_t id = uint32_t(capped.corners.size());
            capped.corners.push_back(corner);
            cornerOf.emplace(vertex, id);
            return id;
        };
        for (size_t t = 0; t < triangles.size(); t += 3)
        {
            MeshFace face;
            face.kind = MeshFaceKind::Cap;
            face.sourceTriangle = kInvalidIndex;
            face.cutId = cutId;
            // Counter-clockwise in (u, v); the positive piece needs the opposite winding.
            const uint32_t a = triangles[t], b = triangles[t + (positiveSide ? 2 : 1)], c = triangles[t + (positiveSide ? 1 : 2)];
            face.corner[0] = cornerFor(a);
            face.corner[1] = cornerFor(b);
            face.corner[2] = cornerFor(c);
            capped.faces.push_back(face);
        }

        // Every cut edge must now be shared by the surface and exactly one cap face, used in
        // opposite directions. Then it is a seam instead of a boundary.
        MeshEdgeUsageMap usage;
        CollectEdgeUsage(capped, usage);
        for (const SliceLoop& loop : loops)
            for (size_t i = 0; i < loop.vertices.size(); ++i)
            {
                const uint32_t a = loop.vertices[i], b = loop.vertices[(i + 1) % loop.vertices.size()];
                const uint64_t key = MakeEdgeKey(a, b);
                const auto use = usage.find(key);
                if (use == usage.end() || use->second.count != 2 || use->second.forward != 1 || use->second.backward != 1)
                    return Fail(detail, "cut edge (" + Num(a) + ", " + Num(b) + ") is not matched by exactly one cap face");
                capped.edgeTags[key] = MeshEdgeTag::CapSeam;
            }

        double capArea = 0.0;
        for (const MeshFace& face : capped.faces)
            if (face.kind == MeshFaceKind::Cap && face.cutId == cutId)
                capArea += FaceArea(capped, face);
        if (std::fabs(capArea - expectedArea) > 1e-4 * std::max(expectedArea, 1e-12))
            return Fail(detail, "cap area " + std::to_string(capArea) + " differs from the loop area " + std::to_string(expectedArea));

        MeshCutRecord record;
        record.cutId = cutId;
        record.planePoint = plane.point;
        record.planeNormal = normal;
        for (int pass = 0; pass < 2; ++pass)
            for (const SliceLoop& loop : loops)
                if ((loop.depth == 0) == (pass == 0))
                {
                    record.rimVertices.insert(record.rimVertices.end(), loop.vertices.begin(), loop.vertices.end());
                    record.loopSizes.push_back(uint32_t(loop.vertices.size()));
                }
        capped.caps.push_back(record);

        std::string why;
        const SliceStatus valid = ValidateMeshGeometry(capped, epsilonScale, &why);
        if (valid != SliceStatus::Ok)
            return Fail(detail, std::string("capped piece is invalid: ") + SliceStatusName(valid) + " (" + why + ")");

        piece = std::move(capped);
        return SliceStatus::Ok;
    }
}
