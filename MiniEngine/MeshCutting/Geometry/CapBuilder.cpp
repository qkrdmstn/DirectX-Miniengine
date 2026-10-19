#include "../pch.h"
#include "CapBuilder.h"

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

        // One vertex of a ring being triangulated. Bridge edges repeat a node, so a node id can
        // appear twice in the combined ring.
        struct RingVertex
        {
            uint32_t node;
            Point2 p;
        };

        std::string Num(uint64_t value) { return std::to_string(value); }

        double Cross(const Point2& o, const Point2& a, const Point2& b)
        {
            return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
        }

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

        double SegmentDistance(const Point2& a, const Point2& b, const Point2& c, const Point2& d)
        {
            const double o1 = Cross(a, b, c), o2 = Cross(a, b, d), o3 = Cross(c, d, a), o4 = Cross(c, d, b);
            if (((o1 > 0.0 && o2 < 0.0) || (o1 < 0.0 && o2 > 0.0)) && ((o3 > 0.0 && o4 < 0.0) || (o3 < 0.0 && o4 > 0.0)))
                return 0.0;
            return std::min(std::min(PointSegmentDistance(a, c, d), PointSegmentDistance(b, c, d)),
                std::min(PointSegmentDistance(c, a, b), PointSegmentDistance(d, a, b)));
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
                        if (a.node == from.node || b.node == from.node || a.node == to.node || b.node == to.node)
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

        // Ear clipping over a counter-clockwise ring that may repeat nodes (bridges). Only
        // strictly convex tips with enough area are clipped, so straight rim nodes stay in the ring
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
                        if (ring[w].node == ring[p].node || ring[w].node == ring[i].node || ring[w].node == ring[q].node)
                            continue;
                        blocked = Cross(ring[p].p, ring[i].p, ring[w].p) >= -minCross
                            && Cross(ring[i].p, ring[q].p, ring[w].p) >= -minCross
                            && Cross(ring[q].p, ring[p].p, ring[w].p) >= -minCross;
                    }
                    if (blocked)
                        continue;
                    triangles.insert(triangles.end(), { ring[p].node, ring[i].node, ring[q].node });
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
            triangles.insert(triangles.end(), { ring[a].node, ring[b].node, ring[c].node });
            return true;
        }

        SliceStatus Fail(std::string* detail, const std::string& message)
        {
            if (detail)
                *detail = message;
            return SliceStatus::CapTriangulationFailed;
        }
    }

    SliceStatus BuildCap(PatchPiece& piece, const SlicePlane& plane, bool positiveSide, uint32_t cutId,
        const std::vector<CutLoop>& loops, float epsilonScale, float uvScale, std::string* detail)
    {
        if (detail)
            detail->clear();
        if (loops.empty())
            return SliceStatus::Ok;
        PlaneBasis basis;
        if (!MakePlaneBasis(plane, basis))
            return Fail(detail, "plane is invalid");
        const double epsilon = ComputePieceRadius(piece) * double(epsilonScale);

        // Loops in plane coordinates, as rings.
        std::vector<std::vector<RingVertex>> rings(loops.size());
        for (size_t l = 0; l < loops.size(); ++l)
            for (uint32_t node : loops[l].nodes)
            {
                if (node >= piece.nodes.size())
                    return Fail(detail, "loop node " + Num(node) + " is out of range");
                const DirectX::XMFLOAT3& pos = piece.nodes[node].position;
                const double d[3] = { double(pos.x) - basis.origin[0], double(pos.y) - basis.origin[1], double(pos.z) - basis.origin[2] };
                rings[l].push_back({ node, { d[0] * basis.u[0] + d[1] * basis.u[1] + d[2] * basis.u[2],
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

        std::vector<uint32_t> triangles;        // node triples, counter-clockwise in (u, v)
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

        // Append the cap: one corner per rim node, outward normal, planar UVs. A copy of the piece
        // is edited first so a failed check leaves the caller's piece untouched.
        PatchPiece capped = piece;
        const double sign = positiveSide ? -1.0 : 1.0;      // cap normal = -n for the positive piece
        const DirectX::XMFLOAT3 normal = { float(sign * basis.n[0]), float(sign * basis.n[1]), float(sign * basis.n[2]) };
        std::unordered_map<uint32_t, uint32_t> cornerOf;
        const auto cornerFor = [&](uint32_t node)
        {
            const auto found = cornerOf.find(node);
            if (found != cornerOf.end())
                return found->second;
            const DirectX::XMFLOAT3& pos = capped.nodes[node].position;
            const double d[3] = { double(pos.x) - basis.origin[0], double(pos.y) - basis.origin[1], double(pos.z) - basis.origin[2] };
            RenderCorner corner;
            corner.node = node;
            corner.normal = normal;
            corner.uv = { float((d[0] * basis.u[0] + d[1] * basis.u[1] + d[2] * basis.u[2]) * double(uvScale)),
                float((d[0] * basis.v[0] + d[1] * basis.v[1] + d[2] * basis.v[2]) * double(uvScale)) };
            corner.color = { 0.9f, 0.9f, 0.9f, 1.0f };
            const uint32_t id = uint32_t(capped.corners.size());
            capped.corners.push_back(corner);
            cornerOf.emplace(node, id);
            return id;
        };
        for (size_t t = 0; t < triangles.size(); t += 3)
        {
            Face face;
            face.kind = FaceKind::Cap;
            face.sourceTriangle = kNoNode;
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
        EdgeUsageMap usage;
        CollectEdgeUsage(capped, usage);
        for (const CutLoop& loop : loops)
            for (size_t i = 0; i < loop.nodes.size(); ++i)
            {
                const uint32_t a = loop.nodes[i], b = loop.nodes[(i + 1) % loop.nodes.size()];
                const uint64_t key = MakeEdgeKey(a, b);
                const auto use = usage.find(key);
                if (use == usage.end() || use->second.count != 2 || use->second.forward != 1 || use->second.backward != 1)
                    return Fail(detail, "cut edge (" + Num(a) + ", " + Num(b) + ") is not matched by exactly one cap face");
                capped.boundary[key] = EdgeKind::CapSeam;
            }

        double capArea = 0.0;
        for (const Face& face : capped.faces)
            if (face.kind == FaceKind::Cap && face.cutId == cutId)
                capArea += FaceArea(capped, face);
        if (std::fabs(capArea - expectedArea) > 1e-4 * std::max(expectedArea, 1e-12))
            return Fail(detail, "cap area " + std::to_string(capArea) + " differs from the loop area " + std::to_string(expectedArea));

        CapRecord record;
        record.cutId = cutId;
        record.planePoint = plane.point;
        record.planeNormal = normal;
        for (int pass = 0; pass < 2; ++pass)
            for (const CutLoop& loop : loops)
                if ((loop.depth == 0) == (pass == 0))
                {
                    record.rimNodes.insert(record.rimNodes.end(), loop.nodes.begin(), loop.nodes.end());
                    record.loopSizes.push_back(uint32_t(loop.nodes.size()));
                }
        capped.caps.push_back(record);

        std::string why;
        const SliceStatus valid = ValidatePatchPiece(capped, epsilonScale, &why);
        if (valid != SliceStatus::Ok)
            return Fail(detail, std::string("capped piece is invalid: ") + SliceStatusName(valid) + " (" + why + ")");

        piece = std::move(capped);
        return SliceStatus::Ok;
    }
}
