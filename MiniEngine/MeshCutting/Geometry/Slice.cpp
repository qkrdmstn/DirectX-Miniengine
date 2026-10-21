// 이 파일은 평면 하나로 패치 조각을 두 조각으로 자르는 슬라이서(SlicePatch)를 구현한다.
#include "../pch.h"
#include "Slice.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace MeshCutting
{
    namespace
    {
        struct CutNodeInfo
        {
            uint32_t low;       // original nodes of the cut edge, low < high
            uint32_t high;
            double t;           // position = low + t * (high - low)
        };

        // One output piece while the slice is running. Corner nodes are global node ids
        // (original nodes first, cut nodes after) until the piece is compacted.
        struct SideBuild
        {
            std::vector<PatchCorner> corners;
            std::vector<PatchFace> faces;
            std::unordered_map<uint32_t, uint32_t> wholeCorner;     // input corner -> corner
            std::unordered_map<uint64_t, uint32_t> cutCorner;       // sorted input corner pair -> corner
        };

        class Slicer
        {
        public:
            Slicer(const PatchPiece& in, const SlicePlane& plane, uint32_t cutId, const SliceOptions& options)
                : m_In(in), m_Plane(plane), m_CutId(cutId), m_Options(options)
            {
            }

            SliceStatus Run(SliceResult& out);

        private:
            bool Fail(SliceStatus status, const std::string& message)
            {
                m_Status = status;
                m_Detail = message;
                return false;
            }

            bool ClassifyNodes();
            bool ProcessFace(uint32_t faceIndex);
            uint32_t CutNodeOf(uint32_t nodeA, uint32_t nodeB);
            uint32_t WholeCorner(int side, uint32_t inputCorner);
            uint32_t CutCorner(int side, uint32_t cornerA, uint32_t cornerB, uint32_t cutNode, uint32_t faceIndex);
            void AddFace(int side, const PatchFace& source, bool split, uint32_t c0, uint32_t c1, uint32_t c2);
            bool BuildPiece(int side, PatchPiece& piece);
            bool InheritedKind(uint32_t globalA, uint32_t globalB, PatchEdgeKind& kind) const;
            bool IsCutNode(uint32_t global) const { return global >= m_In.nodes.size(); }

            const PatchPiece& m_In;
            const SlicePlane& m_Plane;
            uint32_t m_CutId;
            SliceOptions m_Options;

            SlicePlaneBasis m_Basis;
            double m_Epsilon = 0.0;
            std::vector<PatchNode> m_Nodes;          // original nodes, then cut nodes
            std::vector<double> m_Distance;         // signed plane distance of original nodes
            std::vector<int8_t> m_Sign;             // +1, 0, -1; cut nodes are 0
            std::vector<CutNodeInfo> m_CutNodes;
            std::unordered_map<uint64_t, uint32_t> m_EdgeNode;      // geometric edge -> cut node
            SideBuild m_Side[2];                    // 0 = positive, 1 = negative
            std::vector<uint8_t> m_OnPlane[2];      // per output piece node: lies on the plane
            std::unordered_map<uint32_t, int> m_CapSides;   // cap cut id -> bit mask of sides
            SliceStatus m_Status = SliceStatus::InvalidInput;
            std::string m_Detail;
        };

        bool MergeOrigin(const PatchOrigin& low, const PatchOrigin& high, double t, PatchOrigin& out)
        {
            uint32_t id[6];
            double weight[6];
            int count = 0;
            for (int i = 0; i < 3; ++i)
                if (low.skinVertex[i] != kNoNode)
                {
                    id[count] = low.skinVertex[i];
                    weight[count++] = (1.0 - t) * low.bary[i];
                }
            for (int i = 0; i < 3; ++i)
                if (high.skinVertex[i] != kNoNode)
                {
                    id[count] = high.skinVertex[i];
                    weight[count++] = t * high.bary[i];
                }
            for (int i = 1; i < count; ++i)
                for (int j = i; j > 0 && id[j - 1] > id[j]; --j)
                {
                    std::swap(id[j - 1], id[j]);
                    std::swap(weight[j - 1], weight[j]);
                }
            uint32_t mergedId[6];
            double mergedWeight[6];
            int merged = 0;
            for (int i = 0; i < count; ++i)
            {
                if (merged > 0 && mergedId[merged - 1] == id[i])
                    mergedWeight[merged - 1] += weight[i];
                else
                {
                    mergedId[merged] = id[i];
                    mergedWeight[merged++] = weight[i];
                }
            }
            if (merged > 3)
                return false;
            double sum = 0.0;
            for (int i = 0; i < merged; ++i)
                sum += mergedWeight[i];
            out = PatchOrigin();
            for (int i = 0; i < merged; ++i)
            {
                out.skinVertex[i] = mergedId[i];
                out.bary[i] = float(mergedWeight[i] / sum);
            }
            return true;
        }

        void Normalize3(double v[3], const double fallback[3])
        {
            const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (length > 1e-6)
            {
                for (int k = 0; k < 3; ++k)
                    v[k] /= length;
            }
            else
            {
                for (int k = 0; k < 3; ++k)
                    v[k] = fallback[k];
            }
        }

        bool Slicer::ClassifyNodes()
        {
            m_Nodes = m_In.nodes;
            m_Distance.resize(m_In.nodes.size());
            m_Sign.resize(m_In.nodes.size());
            for (size_t i = 0; i < m_In.nodes.size(); ++i)
            {
                const DirectX::XMFLOAT3& p = m_In.nodes[i].position;
                const double d = (double(p.x) - m_Basis.origin[0]) * m_Basis.n[0]
                    + (double(p.y) - m_Basis.origin[1]) * m_Basis.n[1]
                    + (double(p.z) - m_Basis.origin[2]) * m_Basis.n[2];
                m_Distance[i] = d;
                m_Sign[i] = d > m_Epsilon ? 1 : (d < -m_Epsilon ? -1 : 0);
            }
            return true;
        }

        // The node where the plane crosses the geometric edge (nodeA, nodeB). The ends must lie
        // strictly on opposite sides, so t is inside (0, 1) and the node is never an end point.
        uint32_t Slicer::CutNodeOf(uint32_t nodeA, uint32_t nodeB)
        {
            const uint64_t key = MakeEdgeKey(nodeA, nodeB);
            const auto found = m_EdgeNode.find(key);
            if (found != m_EdgeNode.end())
                return found->second;

            const uint32_t low = EdgeKeyLow(key), high = EdgeKeyHigh(key);
            if (m_In.nodes[low].kind != PatchNodeKind::Surface || m_In.nodes[high].kind != PatchNodeKind::Surface)
            {
                Fail(SliceStatus::CrossesCap, "edge (" + Num(low) + ", " + Num(high) + ") touches a cap node");
                return kNoNode;
            }
            const double dl = m_Distance[low], dh = m_Distance[high];
            const double t = dl / (dl - dh);

            PatchNode node;
            node.kind = PatchNodeKind::Surface;
            node.cutId = m_CutId;
            if (!MergeOrigin(m_In.nodes[low].origin, m_In.nodes[high].origin, t, node.origin))
            {
                Fail(SliceStatus::InvalidOrigin, "cut node on edge (" + Num(low) + ", " + Num(high)
                    + ") would depend on more than three original nodes");
                return kNoNode;
            }
            const DirectX::XMFLOAT3& a = m_In.nodes[low].position;
            const DirectX::XMFLOAT3& b = m_In.nodes[high].position;
            node.position = { float(a.x + t * (double(b.x) - a.x)), float(a.y + t * (double(b.y) - a.y)),
                float(a.z + t * (double(b.z) - a.z)) };

            const uint32_t id = uint32_t(m_Nodes.size());
            m_Nodes.push_back(node);
            m_Sign.push_back(0);
            m_CutNodes.push_back({ low, high, t });
            m_EdgeNode.emplace(key, id);
            return id;
        }

        uint32_t Slicer::WholeCorner(int side, uint32_t inputCorner)
        {
            SideBuild& build = m_Side[side];
            const auto found = build.wholeCorner.find(inputCorner);
            if (found != build.wholeCorner.end())
                return found->second;
            const uint32_t id = uint32_t(build.corners.size());
            build.corners.push_back(m_In.corners[inputCorner]);
            build.wholeCorner.emplace(inputCorner, id);
            return id;
        }

        // Attributes are interpolated along the render edge from the corner with the lower node
        // to the one with the higher node, using the same t as the shared cut node.
        uint32_t Slicer::CutCorner(int side, uint32_t cornerA, uint32_t cornerB, uint32_t cutNode, uint32_t faceIndex)
        {
            SideBuild& build = m_Side[side];
            const uint64_t key = MakeEdgeKey(cornerA, cornerB);
            const auto found = build.cutCorner.find(key);
            if (found != build.cutCorner.end())
                return found->second;

            const bool aIsLow = m_In.corners[cornerA].node < m_In.corners[cornerB].node;
            const PatchCorner& low = m_In.corners[aIsLow ? cornerA : cornerB];
            const PatchCorner& high = m_In.corners[aIsLow ? cornerB : cornerA];
            const double t = m_CutNodes[cutNode - m_In.nodes.size()].t;

            PatchCorner corner;
            corner.node = cutNode;
            corner.uv = { float(low.uv.x + t * (double(high.uv.x) - low.uv.x)), float(low.uv.y + t * (double(high.uv.y) - low.uv.y)) };
            corner.color = { float(low.color.x + t * (double(high.color.x) - low.color.x)),
                float(low.color.y + t * (double(high.color.y) - low.color.y)),
                float(low.color.z + t * (double(high.color.z) - low.color.z)),
                float(low.color.w + t * (double(high.color.w) - low.color.w)) };
            double n[3] = { low.normal.x + t * (double(high.normal.x) - low.normal.x),
                low.normal.y + t * (double(high.normal.y) - low.normal.y),
                low.normal.z + t * (double(high.normal.z) - low.normal.z) };
            if (std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) <= 1e-6)
            {
                // Opposite normals cancel; fall back to the face normal.
                const PatchFace& face = m_In.faces[faceIndex];
                const DirectX::XMFLOAT3& p0 = m_In.nodes[m_In.corners[face.corner[0]].node].position;
                const DirectX::XMFLOAT3& p1 = m_In.nodes[m_In.corners[face.corner[1]].node].position;
                const DirectX::XMFLOAT3& p2 = m_In.nodes[m_In.corners[face.corner[2]].node].position;
                const double e1[3] = { double(p1.x) - p0.x, double(p1.y) - p0.y, double(p1.z) - p0.z };
                const double e2[3] = { double(p2.x) - p0.x, double(p2.y) - p0.y, double(p2.z) - p0.z };
                n[0] = e1[1] * e2[2] - e1[2] * e2[1];
                n[1] = e1[2] * e2[0] - e1[0] * e2[2];
                n[2] = e1[0] * e2[1] - e1[1] * e2[0];
            }
            const double fallback[3] = { 0.0, 1.0, 0.0 };
            Normalize3(n, fallback);
            corner.normal = { float(n[0]), float(n[1]), float(n[2]) };

            const uint32_t id = uint32_t(build.corners.size());
            build.corners.push_back(corner);
            build.cutCorner.emplace(key, id);
            return id;
        }

        void Slicer::AddFace(int side, const PatchFace& source, bool split, uint32_t c0, uint32_t c1, uint32_t c2)
        {
            PatchFace face = source;
            if (split)
                face.cutId = m_CutId;           // a surface face this cut divided (cap faces are never split)
            face.corner[0] = c0;
            face.corner[1] = c1;
            face.corner[2] = c2;
            m_Side[side].faces.push_back(face);
            if (source.kind == PatchFaceKind::Cap)
                m_CapSides[source.cutId] |= 1 << side;
        }

        bool Slicer::ProcessFace(uint32_t faceIndex)
        {
            const PatchFace& face = m_In.faces[faceIndex];
            uint32_t c[3], n[3];
            int8_t s[3];
            int positive = 0, negative = 0, zero = 0;
            for (int k = 0; k < 3; ++k)
            {
                c[k] = face.corner[k];
                n[k] = m_In.corners[c[k]].node;
                s[k] = m_Sign[n[k]];
                positive += s[k] > 0;
                negative += s[k] < 0;
                zero += s[k] == 0;
            }

            if (positive + negative == 0)
                return Fail(SliceStatus::CoplanarFace, "face " + Num(faceIndex) + " lies in the plane");

            const auto whole = [&](int side)
            {
                AddFace(side, face, false, WholeCorner(side, c[0]), WholeCorner(side, c[1]), WholeCorner(side, c[2]));
            };
            if (negative == 0)
            {
                whole(0);
                return true;
            }
            if (positive == 0)
            {
                whole(1);
                return true;
            }
            if (face.kind == PatchFaceKind::Cap)
                return Fail(SliceStatus::CrossesCap, "the plane cuts cap face " + Num(faceIndex) + " of cut " + Num(face.cutId));

            if (zero == 1)
            {
                // (Z, A, B) with A and B strictly on opposite sides: split the A-B edge at P.
                int z = 0;
                while (s[z] != 0)
                    ++z;
                const int a = (z + 1) % 3, b = (z + 2) % 3;
                const uint32_t p = CutNodeOf(n[a], n[b]);
                if (p == kNoNode)
                    return false;
                const int sideA = s[a] > 0 ? 0 : 1, sideB = 1 - sideA;
                const uint32_t cornerP[2] = { CutCorner(0, c[a], c[b], p, faceIndex), CutCorner(1, c[a], c[b], p, faceIndex) };
                AddFace(sideA, face, true, WholeCorner(sideA, c[z]), WholeCorner(sideA, c[a]), cornerP[sideA]);
                AddFace(sideB, face, true, WholeCorner(sideB, c[z]), cornerP[sideB], WholeCorner(sideB, c[b]));
                return true;
            }

            // Two corners on one side and one on the other: the lone corner L keeps a triangle,
            // the other side gets the quad (P1, X, Y, P2) as two triangles. Winding is preserved.
            const int lone = s[0] == s[1] ? 2 : (s[0] == s[2] ? 1 : 0);
            const int x = (lone + 1) % 3, y = (lone + 2) % 3;
            const uint32_t p1 = CutNodeOf(n[lone], n[x]);
            if (p1 == kNoNode)
                return false;
            const uint32_t p2 = CutNodeOf(n[y], n[lone]);
            if (p2 == kNoNode)
                return false;
            const int loneSide = s[lone] > 0 ? 0 : 1, otherSide = 1 - loneSide;
            const uint32_t corner1[2] = { CutCorner(0, c[lone], c[x], p1, faceIndex), CutCorner(1, c[lone], c[x], p1, faceIndex) };
            const uint32_t corner2[2] = { CutCorner(0, c[y], c[lone], p2, faceIndex), CutCorner(1, c[y], c[lone], p2, faceIndex) };
            AddFace(loneSide, face, true, WholeCorner(loneSide, c[lone]), corner1[loneSide], corner2[loneSide]);
            AddFace(otherSide, face, true, corner1[otherSide], WholeCorner(otherSide, c[x]), WholeCorner(otherSide, c[y]));
            AddFace(otherSide, face, true, corner1[otherSide], WholeCorner(otherSide, c[y]), corner2[otherSide]);
            return true;
        }

        // The kind an edge of the output inherits from the input piece. A half of a split edge
        // carries the kind of the whole edge; edges created by this cut have none.
        bool Slicer::InheritedKind(uint32_t globalA, uint32_t globalB, PatchEdgeKind& kind) const
        {
            uint32_t a = globalA, b = globalB;
            const bool newA = IsCutNode(globalA), newB = IsCutNode(globalB);
            if (newA && newB)
                return false;
            if (newA || newB)
            {
                const uint32_t cut = newA ? globalA : globalB;
                const uint32_t old = newA ? globalB : globalA;
                const CutNodeInfo& info = m_CutNodes[cut - m_In.nodes.size()];
                if (old != info.low && old != info.high)
                    return false;
                a = info.low;
                b = info.high;
            }
            const auto found = m_In.boundary.find(MakeEdgeKey(a, b));
            if (found == m_In.boundary.end())
                return false;
            kind = found->second;
            return true;
        }

        bool Slicer::BuildPiece(int side, PatchPiece& piece)
        {
            SideBuild& build = m_Side[side];
            piece = PatchPiece();
            piece.skin = m_In.skin;
            piece.skinStateId = m_In.skinStateId;

            // Keep the nodes the faces use, in ascending global order.
            std::vector<uint8_t> used(m_Nodes.size(), 0);
            for (const PatchCorner& corner : build.corners)
                used[corner.node] = 1;
            std::vector<uint32_t> remap(m_Nodes.size(), kNoNode);
            std::vector<uint32_t> globalOf;
            for (uint32_t g = 0; g < m_Nodes.size(); ++g)
                if (used[g])
                {
                    remap[g] = uint32_t(piece.nodes.size());
                    piece.nodes.push_back(m_Nodes[g]);
                    globalOf.push_back(g);
                }
            piece.corners = build.corners;
            for (PatchCorner& corner : piece.corners)
                corner.node = remap[corner.node];
            piece.faces = build.faces;
            m_OnPlane[side].assign(piece.nodes.size(), 0);
            for (size_t i = 0; i < globalOf.size(); ++i)
                m_OnPlane[side][i] = m_Sign[globalOf[i]] == 0 ? 1 : 0;

            // Edge kinds are rebuilt from this piece's adjacency (design 1.7).
            PatchEdgeUsageMap usage;
            CollectEdgeUsage(piece, usage);
            std::vector<uint64_t> keys;
            keys.reserve(usage.size());
            for (const auto& entry : usage)
                keys.push_back(entry.first);
            std::sort(keys.begin(), keys.end());
            for (uint64_t key : keys)
            {
                const PatchEdgeUsage& use = usage[key];
                const uint32_t ga = globalOf[EdgeKeyLow(key)], gb = globalOf[EdgeKeyHigh(key)];
                PatchEdgeKind inherited = PatchEdgeKind::Interior;
                const bool has = InheritedKind(ga, gb, inherited);
                if (use.count == 1)
                {
                    if (has && inherited != PatchEdgeKind::CapSeam)
                        piece.boundary[key] = inherited;
                    else
                    {
                        // A new boundary can only lie in the plane; anything else is a bug here.
                        if (m_Sign[ga] != 0 || m_Sign[gb] != 0)
                            return Fail(SliceStatus::InvalidInput, "internal: new boundary edge (" + Num(ga) + ", "
                                + Num(gb) + ") is off the plane");
                        piece.boundary[key] = PatchEdgeKind::Cut;
                    }
                }
                else if (use.count == 2 && has && inherited == PatchEdgeKind::CapSeam)
                    piece.boundary[key] = PatchEdgeKind::CapSeam;
            }

            // Cap records follow their cap faces.
            for (const PatchCap& cap : m_In.caps)
            {
                bool present = false;
                for (const PatchFace& face : piece.faces)
                    if (face.kind == PatchFaceKind::Cap && face.cutId == cap.cutId)
                    {
                        present = true;
                        break;
                    }
                if (!present)
                    continue;
                PatchCap moved = cap;
                for (uint32_t& rim : moved.rimNodes)
                {
                    if (rim >= remap.size() || remap[rim] == kNoNode)
                        return Fail(SliceStatus::InvalidInput, "internal: rim node " + Num(rim) + " of cut " + Num(cap.cutId) + " was dropped");
                    rim = remap[rim];
                }
                piece.caps.push_back(moved);
            }
            return true;
        }

        SliceStatus Slicer::Run(SliceResult& out)
        {
            std::string why;
            SliceStatus status = ValidatePlane(m_Plane, &why);
            if (status == SliceStatus::Ok)
                status = ValidatePatchPiece(m_In, m_Options.epsilonScale, &why);
            if (status != SliceStatus::Ok)
            {
                out.status = status;
                out.detail = why;
                return status;
            }
            MakePlaneBasis(m_Plane, m_Basis);
            m_Epsilon = ComputePieceRadius(m_In) * double(m_Options.epsilonScale);
            ClassifyNodes();

            for (uint32_t f = 0; f < m_In.faces.size(); ++f)
                if (!ProcessFace(f))
                {
                    out.status = m_Status;
                    out.detail = m_Detail;
                    return m_Status;
                }
            for (const auto& entry : m_CapSides)
                if (entry.second == 3)
                {
                    out.status = SliceStatus::CrossesCap;
                    out.detail = "cap of cut " + Num(entry.first) + " ends up on both sides of the plane";
                    return out.status;
                }
            if (m_Side[0].faces.empty() || m_Side[1].faces.empty())
            {
                out.status = SliceStatus::NoCut;
                out.detail = m_Side[0].faces.empty() ? "every face is on the negative side" : "every face is on the positive side";
                return out.status;
            }

            PatchPiece pieces[2];
            for (int side = 0; side < 2; ++side)
            {
                if (!BuildPiece(side, pieces[side]))
                {
                    out.status = m_Status;
                    out.detail = m_Detail;
                    return m_Status;
                }
                // A result must be a valid input for the next cut. Clipping can leave slivers
                // below the degeneracy threshold of the piece (for example a face that was just
                // above it), and those are rejected instead of published.
                std::string outputWhy;
                const SliceStatus outputStatus = ValidatePatchPiece(pieces[side], m_Options.epsilonScale, &outputWhy);
                if (outputStatus != SliceStatus::Ok)
                {
                    out.status = outputStatus;
                    out.detail = std::string("output ") + (side == 0 ? "positive" : "negative") + " piece: " + outputWhy;
                    return outputStatus;
                }
            }

            // Cut loops come from each half's own boundary and must agree between the halves.
            std::vector<SliceLoop> loops[2];
            for (int side = 0; side < 2; ++side)
            {
                std::string loopWhy;
                const SliceStatus loopStatus = ExtractCutLoops(pieces[side], m_OnPlane[side], m_Plane, side == 0,
                    m_Options.epsilonScale, loops[side], &loopWhy);
                if (loopStatus != SliceStatus::Ok)
                {
                    out.status = loopStatus;
                    out.detail = std::string(side == 0 ? "positive" : "negative") + " piece: " + loopWhy;
                    return loopStatus;
                }
            }
            std::vector<size_t> sizes[2];
            for (int side = 0; side < 2; ++side)
            {
                for (const SliceLoop& loop : loops[side])
                    sizes[side].push_back(loop.nodes.size());
                std::sort(sizes[side].begin(), sizes[side].end());
            }
            if (sizes[0] != sizes[1])
            {
                out.status = SliceStatus::InvalidLoop;
                out.detail = "internal: the two pieces disagree on the cut loops (" + Num(loops[0].size()) + " vs "
                    + Num(loops[1].size()) + ")";
                return out.status;
            }

            if (m_Options.cap == CapMode::Create)
            {
                for (int side = 0; side < 2; ++side)
                {
                    std::string capWhy;
                    const SliceStatus capStatus = BuildCap(pieces[side], m_Plane, side == 0, m_CutId, loops[side],
                        m_Options.epsilonScale, m_Options.capUvScale, &capWhy);
                    if (capStatus != SliceStatus::Ok)
                    {
                        out.status = capStatus;
                        out.detail = std::string(side == 0 ? "positive" : "negative") + " cap: " + capWhy;
                        return capStatus;
                    }
                }
            }

            out.positive = std::move(pieces[0]);
            out.negative = std::move(pieces[1]);
            for (const SliceLoop& loop : loops[0])
            {
                out.loops.push_back(loop.nodes);
                out.loopDepth.push_back(loop.depth);
                out.loopArea.push_back(loop.signedArea);
            }
            out.status = SliceStatus::Ok;
            out.detail.clear();
            return SliceStatus::Ok;
        }
    }

    SliceStatus SlicePatch(const PatchPiece& in, const SlicePlane& plane, uint32_t cutId,
        const SliceOptions& options, SliceResult& out)
    {
        out = SliceResult();
        Slicer slicer(in, plane, cutId, options);
        const SliceStatus status = slicer.Run(out);
        if (status != SliceStatus::Ok)
        {
            // Failure contract: only status and detail are filled.
            out.positive.Clear();
            out.negative.Clear();
            out.loops.clear();
            out.loopDepth.clear();
            out.loopArea.clear();
        }
        return status;
    }
}
