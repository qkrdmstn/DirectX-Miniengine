#include "../pch.h"
#include "CutLoops.h"

#include <algorithm>
#include <cmath>

namespace MeshCutting
{
    namespace
    {
        struct Point2
        {
            double x, y;
        };

        std::string Num(uint64_t value) { return std::to_string(value); }

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

        SliceStatus Fail(std::string* detail, SliceStatus status, const std::string& message)
        {
            if (detail)
                *detail = message;
            return status;
        }
    }

    SliceStatus ExtractCutLoops(const PatchPiece& piece, const std::vector<uint8_t>& onPlane,
        const SlicePlane& plane, bool positiveSide, float epsilonScale,
        std::vector<CutLoop>& loops, std::string* detail)
    {
        loops.clear();
        if (detail)
            detail->clear();
        PlaneBasis basis;
        if (!MakePlaneBasis(plane, basis))
            return Fail(detail, SliceStatus::InvalidInput, "plane is invalid");
        if (onPlane.size() != piece.nodes.size())
            return Fail(detail, SliceStatus::InvalidInput, "onPlane does not match the node count");
        const double epsilon = ComputePieceRadius(piece) * double(epsilonScale);

        // Directed cut edges from the faces: each boundary edge belongs to exactly one face.
        const size_t nodeCount = piece.nodes.size();
        std::vector<uint32_t> outCount(nodeCount, 0), inCount(nodeCount, 0), next(nodeCount, kNoNode);
        for (const Face& face : piece.faces)
        {
            for (int k = 0; k < 3; ++k)
            {
                const uint32_t from = piece.corners[face.corner[k]].node;
                const uint32_t to = piece.corners[face.corner[(k + 1) % 3]].node;
                const auto tag = piece.boundary.find(MakeEdgeKey(from, to));
                if (tag == piece.boundary.end() || tag->second != EdgeKind::Cut || !onPlane[from] || !onPlane[to])
                    continue;
                ++outCount[from];
                ++inCount[to];
                next[from] = to;
            }
        }

        // What else the boundary touches at a node, in key order so the message is stable.
        std::vector<int> blocker(nodeCount, -1);
        std::vector<uint64_t> keys;
        keys.reserve(piece.boundary.size());
        for (const auto& entry : piece.boundary)
            keys.push_back(entry.first);
        std::sort(keys.begin(), keys.end());
        for (uint64_t key : keys)
        {
            const EdgeKind kind = piece.boundary.at(key);
            if (kind == EdgeKind::CapSeam)
                continue;
            const uint32_t a = EdgeKeyLow(key), b = EdgeKeyHigh(key);
            if (a >= nodeCount || b >= nodeCount)
                continue;
            if (kind == EdgeKind::Cut && onPlane[a] && onPlane[b])
                continue;       // a cut edge of this plane, not a blocker
            if (blocker[a] < 0)
                blocker[a] = int(kind);
            if (blocker[b] < 0)
                blocker[b] = int(kind);
        }

        // Degree check: open ends caused by another boundary are OpenLoop, the rest NonManifoldLoop.
        uint32_t openNode = kNoNode, badNode = kNoNode;
        for (uint32_t n = 0; n < nodeCount; ++n)
        {
            const uint32_t out = outCount[n], in = inCount[n];
            if (out == 0 && in == 0)
                continue;
            if (out == 1 && in == 1)
                continue;
            if (out + in == 1 && blocker[n] >= 0)
            {
                if (openNode == kNoNode)
                    openNode = n;
            }
            else if (badNode == kNoNode)
                badNode = n;
        }
        if (badNode != kNoNode)
            return Fail(detail, SliceStatus::NonManifoldLoop, "cut node " + Num(badNode) + " has " + Num(outCount[badNode])
                + " outgoing and " + Num(inCount[badNode]) + " incoming cut edges");
        if (openNode != kNoNode)
            return Fail(detail, SliceStatus::OpenLoop, "cut loop ends at node " + Num(openNode) + " on a "
                + EdgeKindName(EdgeKind(blocker[openNode])) + " boundary");

        // Every remaining cut node has one way in and one way out, so the cut edges are cycles.
        std::vector<uint8_t> visited(nodeCount, 0);
        for (uint32_t start = 0; start < nodeCount; ++start)
        {
            if (outCount[start] != 1 || visited[start])
                continue;
            CutLoop loop;
            uint32_t node = start;
            do
            {
                visited[node] = 1;
                loop.nodes.push_back(node);
                node = next[node];
            } while (node != start);
            if (loop.nodes.size() < 3)
                return Fail(detail, SliceStatus::InvalidLoop, "loop through node " + Num(start) + " has only "
                    + Num(loop.nodes.size()) + " nodes");
            loops.push_back(std::move(loop));
        }
        if (loops.empty())
            return SliceStatus::Ok;

        // Project into the plane.
        std::vector<std::vector<Point2>> points(loops.size());
        for (size_t l = 0; l < loops.size(); ++l)
        {
            for (uint32_t n : loops[l].nodes)
            {
                const DirectX::XMFLOAT3& p = piece.nodes[n].position;
                const double d[3] = { double(p.x) - basis.origin[0], double(p.y) - basis.origin[1], double(p.z) - basis.origin[2] };
                points[l].push_back({ d[0] * basis.u[0] + d[1] * basis.u[1] + d[2] * basis.u[2],
                    d[0] * basis.v[0] + d[1] * basis.v[1] + d[2] * basis.v[2] });
            }
        }

        // Segments of one loop may only meet at their shared node; loops may not meet at all.
        for (size_t a = 0; a < loops.size(); ++a)
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
                        return Fail(detail, SliceStatus::InvalidLoop, "loop " + Num(a) + " crosses or touches itself between nodes "
                            + Num(loops[a].nodes[i]) + " and " + Num(loops[a].nodes[j]));
                }
                for (size_t b = a + 1; b < loops.size(); ++b)
                {
                    const size_t nb = points[b].size();
                    for (size_t j = 0; j < nb; ++j)
                        if (SegmentDistance(a0, a1, points[b][j], points[b][(j + 1) % nb]) <= epsilon)
                            return Fail(detail, SliceStatus::InvalidLoop, "loops " + Num(a) + " and " + Num(b) + " cross or touch near nodes "
                                + Num(loops[a].nodes[i]) + " and " + Num(loops[b].nodes[j]));
                }
            }
        }

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
                return Fail(detail, SliceStatus::InvalidLoop, "loop " + Num(l) + " has no area");
        }
        for (size_t l = 0; l < loops.size(); ++l)
        {
            uint32_t depth = 0;
            uint32_t parent = kNoNode;
            for (size_t m = 0; m < loops.size(); ++m)
            {
                if (m == l || std::fabs(loops[m].signedArea) <= std::fabs(loops[l].signedArea))
                    continue;
                if (!PointInPolygon(points[l][0], points[m]))
                    continue;
                ++depth;
                if (parent == kNoNode || std::fabs(loops[m].signedArea) < std::fabs(loops[parent].signedArea))
                    parent = uint32_t(m);
            }
            if (depth > 1)
                return Fail(detail, SliceStatus::InvalidLoop, "loop " + Num(l) + " is nested " + Num(depth)
                    + " deep; islands inside holes are not supported");
            loops[l].depth = depth;
            loops[l].parent = parent;
        }
        const double outerSign = positiveSide ? 1.0 : -1.0;
        for (size_t l = 0; l < loops.size(); ++l)
        {
            const double expected = loops[l].depth == 0 ? outerSign : -outerSign;
            if (loops[l].signedArea * expected <= 0.0)
                return Fail(detail, SliceStatus::InvalidLoop, "loop " + Num(l) + " winds the wrong way for "
                    + (loops[l].depth == 0 ? "an outer boundary" : "a hole"));
        }
        return SliceStatus::Ok;
    }
}
