// 이 파일은 평면 하나로 패치 조각을 두 조각으로 자르는 슬라이서(SliceGeometry)를 구현한다.
#include "../pch.h"
#include "Slice.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace MeshCutting
{
    namespace
    {
        struct CutVertexInfo
        {
            uint32_t low;       // original vertices of the cut edge, low < high
            uint32_t high;
            double t;           // position = low + t * (high - low)
        };

        // One output piece while the slice is running. Corner vertices are global vertex ids
        // (original vertices first, cut vertices after) until the piece is compacted.
        struct SideBuild
        {
            std::vector<MeshCorner> corners;
            std::vector<MeshFace> faces;
            std::unordered_map<uint32_t, uint32_t> wholeCorner;     // input corner -> corner
            std::unordered_map<uint64_t, uint32_t> cutCorner;       // sorted input corner pair -> corner
        };

        class Slicer
        {
        public:
            Slicer(const MeshGeometry& in, const SlicePlane& plane, uint32_t cutId, const SliceOptions& options)
                : m_In(in), m_Plane(plane), m_CutId(cutId), m_Options(options)
            {
            }

            SliceStatus Run(GeometrySliceResult& out);

        private:
            bool Fail(SliceStatus status, const std::string& message)
            {
                m_Status = status;
                m_Detail = message;
                return false;
            }

            bool ClassifyVertices();
            bool ProcessFace(uint32_t faceIndex);
            uint32_t CutVertexOf(uint32_t vertexA, uint32_t vertexB);
            uint32_t WholeCorner(int side, uint32_t inputCorner);
            uint32_t CutCorner(int side, uint32_t cornerA, uint32_t cornerB, uint32_t cutNode, uint32_t faceIndex);
            void AddFace(int side, const MeshFace& source, bool split, uint32_t c0, uint32_t c1, uint32_t c2);
            bool BuildPiece(int side, MeshGeometry& piece);
            bool InheritedKind(uint32_t globalA, uint32_t globalB, MeshEdgeTag& kind) const;
            void CapWhatFits(MeshGeometry& piece, bool positiveSide, std::vector<SliceLoop>& loops,
                std::vector<std::vector<uint32_t>>& uncapped) const;
            bool IsCutVertex(uint32_t global) const { return global >= m_In.vertices.size(); }

            const MeshGeometry& m_In;
            const SlicePlane& m_Plane;
            uint32_t m_CutId;
            SliceOptions m_Options;

            SlicePlaneBasis m_Basis;
            double m_Epsilon = 0.0;
            std::vector<MeshVertex> m_Vertices;          // original vertices, then cut vertices
            std::vector<double> m_Distance;         // signed plane distance of original vertices
            std::vector<int8_t> m_Sign;             // +1, 0, -1; cut vertices are 0
            std::vector<CutVertexInfo> m_CutVertices;
            std::unordered_map<uint64_t, uint32_t> m_EdgeCutVertex;      // geometric edge -> cut vertex
            SideBuild m_Side[2];                    // 0 = positive, 1 = negative
            std::vector<uint8_t> m_OnPlane[2];      // per output piece vertex: lies on the plane
            std::unordered_map<uint32_t, int> m_CapSides;   // cap cut id -> bit mask of sides
            SliceStatus m_Status = SliceStatus::InvalidInput;
            std::string m_Detail;
        };

        bool MergeOrigin(const MeshVertexOrigin& low, const MeshVertexOrigin& high, double t, MeshVertexOrigin& out)
        {
            uint32_t id[6];
            double weight[6];
            int count = 0;
            for (int i = 0; i < 3; ++i)
                if (low.skinVertex[i] != kInvalidIndex)
                {
                    id[count] = low.skinVertex[i];
                    weight[count++] = (1.0 - t) * low.bary[i];
                }
            for (int i = 0; i < 3; ++i)
                if (high.skinVertex[i] != kInvalidIndex)
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
            out = MeshVertexOrigin();
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

        bool Slicer::ClassifyVertices()
        {
            m_Vertices = m_In.vertices;
            m_Distance.resize(m_In.vertices.size());
            m_Sign.resize(m_In.vertices.size());
            for (size_t i = 0; i < m_In.vertices.size(); ++i)
            {
                const DirectX::XMFLOAT3& p = m_In.vertices[i].position;
                const double d = (double(p.x) - m_Basis.origin[0]) * m_Basis.n[0]
                    + (double(p.y) - m_Basis.origin[1]) * m_Basis.n[1]
                    + (double(p.z) - m_Basis.origin[2]) * m_Basis.n[2];
                m_Distance[i] = d;
                m_Sign[i] = d > m_Epsilon ? 1 : (d < -m_Epsilon ? -1 : 0);
            }
            return true;
        }

        // The vertex where the plane crosses the geometric edge (vertexA, vertexB). The ends must lie
        // strictly on opposite sides, so t is inside (0, 1) and the vertex is never an end point.
        uint32_t Slicer::CutVertexOf(uint32_t vertexA, uint32_t vertexB)
        {
            const uint64_t key = MakeEdgeKey(vertexA, vertexB);
            const auto found = m_EdgeCutVertex.find(key);
            if (found != m_EdgeCutVertex.end())
                return found->second;

            const uint32_t low = EdgeKeyLow(key), high = EdgeKeyHigh(key);
            if (m_In.vertices[low].kind != MeshVertexKind::Surface || m_In.vertices[high].kind != MeshVertexKind::Surface)
            {
                Fail(SliceStatus::CrossesCap, "edge (" + Num(low) + ", " + Num(high) + ") touches a cap vertex");
                return kInvalidIndex;
            }
            const double dl = m_Distance[low], dh = m_Distance[high];
            const double t = dl / (dl - dh);

            MeshVertex vertex;
            vertex.kind = MeshVertexKind::Surface;
            vertex.cutId = m_CutId;
            if (!MergeOrigin(m_In.vertices[low].origin, m_In.vertices[high].origin, t, vertex.origin))
            {
                Fail(SliceStatus::InvalidOrigin, "cut vertex on edge (" + Num(low) + ", " + Num(high)
                    + ") would depend on more than three original vertices");
                return kInvalidIndex;
            }
            const DirectX::XMFLOAT3& a = m_In.vertices[low].position;
            const DirectX::XMFLOAT3& b = m_In.vertices[high].position;
            vertex.position = { float(a.x + t * (double(b.x) - a.x)), float(a.y + t * (double(b.y) - a.y)),
                float(a.z + t * (double(b.z) - a.z)) };

            const uint32_t id = uint32_t(m_Vertices.size());
            m_Vertices.push_back(vertex);
            m_Sign.push_back(0);
            m_CutVertices.push_back({ low, high, t });
            m_EdgeCutVertex.emplace(key, id);
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

        // Attributes are interpolated along the render edge from the corner with the lower vertex
        // to the one with the higher vertex, using the same t as the shared cut vertex.
        uint32_t Slicer::CutCorner(int side, uint32_t cornerA, uint32_t cornerB, uint32_t cutNode, uint32_t faceIndex)
        {
            SideBuild& build = m_Side[side];
            const uint64_t key = MakeEdgeKey(cornerA, cornerB);
            const auto found = build.cutCorner.find(key);
            if (found != build.cutCorner.end())
                return found->second;

            const bool aIsLow = m_In.corners[cornerA].vertex < m_In.corners[cornerB].vertex;
            const MeshCorner& low = m_In.corners[aIsLow ? cornerA : cornerB];
            const MeshCorner& high = m_In.corners[aIsLow ? cornerB : cornerA];
            const double t = m_CutVertices[cutNode - m_In.vertices.size()].t;

            MeshCorner corner;
            corner.vertex = cutNode;
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
                const MeshFace& face = m_In.faces[faceIndex];
                const DirectX::XMFLOAT3& p0 = m_In.vertices[m_In.corners[face.corner[0]].vertex].position;
                const DirectX::XMFLOAT3& p1 = m_In.vertices[m_In.corners[face.corner[1]].vertex].position;
                const DirectX::XMFLOAT3& p2 = m_In.vertices[m_In.corners[face.corner[2]].vertex].position;
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

        void Slicer::AddFace(int side, const MeshFace& source, bool split, uint32_t c0, uint32_t c1, uint32_t c2)
        {
            MeshFace face = source;
            if (split)
                face.cutId = m_CutId;           // a surface face this cut divided (cap faces are never split)
            face.corner[0] = c0;
            face.corner[1] = c1;
            face.corner[2] = c2;
            m_Side[side].faces.push_back(face);
            if (source.kind == MeshFaceKind::Cap)
                m_CapSides[source.cutId] |= 1 << side;
        }

        bool Slicer::ProcessFace(uint32_t faceIndex)
        {
            const MeshFace& face = m_In.faces[faceIndex];
            uint32_t c[3], n[3];
            int8_t s[3];
            int positive = 0, negative = 0, zero = 0;
            for (int k = 0; k < 3; ++k)
            {
                c[k] = face.corner[k];
                n[k] = m_In.corners[c[k]].vertex;
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
            if (face.kind == MeshFaceKind::Cap)
                return Fail(SliceStatus::CrossesCap, "the plane cuts cap face " + Num(faceIndex) + " of cut " + Num(face.cutId));

            if (zero == 1)
            {
                // (Z, A, B) with A and B strictly on opposite sides: split the A-B edge at P.
                int z = 0;
                while (s[z] != 0)
                    ++z;
                const int a = (z + 1) % 3, b = (z + 2) % 3;
                const uint32_t p = CutVertexOf(n[a], n[b]);
                if (p == kInvalidIndex)
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
            const uint32_t p1 = CutVertexOf(n[lone], n[x]);
            if (p1 == kInvalidIndex)
                return false;
            const uint32_t p2 = CutVertexOf(n[y], n[lone]);
            if (p2 == kInvalidIndex)
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
        // Unreal SliceProceduralMesh는 다각형마다 따로 삼각분할하고 실패한 다각형의 cap만 버린다. 여기서는 바깥 루프와 그 구멍을
        // 한 묶음으로 본다: 전부 한 번에 되면 그대로, 아니면 묶음마다 시험해서 되는 묶음만 cap을 만들고 나머지는 uncapped로 보낸다.
        // loops는 cap을 만든 루프만 남는다. piece는 cap이 하나도 안 되면 바뀌지 않는다.
        void Slicer::CapWhatFits(MeshGeometry& piece, bool positiveSide, std::vector<SliceLoop>& loops,
            std::vector<std::vector<uint32_t>>& uncapped) const
        {
            if (loops.empty() || BuildCap(piece, m_Plane, positiveSide, m_CutId, loops, m_Options.epsilonScale,
                m_Options.capUvScale) == SliceStatus::Ok)
                return;

            // 묶음 = 바깥 루프 하나 + 그 구멍들 (parent는 묶음 안 번호)
            const auto groupOf = [&](size_t outer)
            {
                std::vector<SliceLoop> group(1, loops[outer]);
                group[0].parent = kInvalidIndex;
                for (const SliceLoop& loop : loops)
                    if (loop.depth == 1 && loop.parent == outer)
                    {
                        group.push_back(loop);
                        group.back().parent = 0;
                    }
                return group;
            };
            std::vector<SliceLoop> fits;
            for (size_t l = 0; l < loops.size(); ++l)
            {
                if (loops[l].depth != 0)
                    continue;
                std::vector<SliceLoop> group = groupOf(l);
                MeshGeometry probe = piece;
                const bool ok = BuildCap(probe, m_Plane, positiveSide, m_CutId, group, m_Options.epsilonScale,
                    m_Options.capUvScale) == SliceStatus::Ok;
                const uint32_t base = uint32_t(fits.size());
                for (SliceLoop& loop : group)
                {
                    if (ok)
                    {
                        if (loop.depth == 1)
                            loop.parent = base;
                        fits.push_back(std::move(loop));
                    }
                    else
                    {
                        loop.vertices.push_back(loop.vertices.front());     // 닫힌 루프: 닫는 에지까지 연속한 쌍
                        uncapped.push_back(std::move(loop.vertices));
                    }
                }
            }
            if (!fits.empty() && BuildCap(piece, m_Plane, positiveSide, m_CutId, fits, m_Options.epsilonScale,
                m_Options.capUvScale) != SliceStatus::Ok)
            {
                // 묶음마다는 됐지만 합치면 안 되는 경우(예상 밖)는 모두 cap 없이 둔다
                for (SliceLoop& loop : fits)
                {
                    loop.vertices.push_back(loop.vertices.front());
                    uncapped.push_back(std::move(loop.vertices));
                }
                fits.clear();
            }
            loops = std::move(fits);
        }

        bool Slicer::InheritedKind(uint32_t globalA, uint32_t globalB, MeshEdgeTag& kind) const
        {
            uint32_t a = globalA, b = globalB;
            const bool newA = IsCutVertex(globalA), newB = IsCutVertex(globalB);
            if (newA && newB)
                return false;
            if (newA || newB)
            {
                const uint32_t cut = newA ? globalA : globalB;
                const uint32_t old = newA ? globalB : globalA;
                const CutVertexInfo& info = m_CutVertices[cut - m_In.vertices.size()];
                if (old != info.low && old != info.high)
                    return false;
                a = info.low;
                b = info.high;
            }
            const auto found = m_In.edgeTags.find(MakeEdgeKey(a, b));
            if (found == m_In.edgeTags.end())
                return false;
            kind = found->second;
            return true;
        }

        bool Slicer::BuildPiece(int side, MeshGeometry& piece)
        {
            SideBuild& build = m_Side[side];
            piece = MeshGeometry();
            piece.skin = m_In.skin;
            piece.skinStateId = m_In.skinStateId;

            // Keep the vertices the faces use, in ascending global order.
            std::vector<uint8_t> used(m_Vertices.size(), 0);
            for (const MeshCorner& corner : build.corners)
                used[corner.vertex] = 1;
            std::vector<uint32_t> remap(m_Vertices.size(), kInvalidIndex);
            std::vector<uint32_t> globalOf;
            for (uint32_t g = 0; g < m_Vertices.size(); ++g)
                if (used[g])
                {
                    remap[g] = uint32_t(piece.vertices.size());
                    piece.vertices.push_back(m_Vertices[g]);
                    globalOf.push_back(g);
                }
            piece.corners = build.corners;
            for (MeshCorner& corner : piece.corners)
                corner.vertex = remap[corner.vertex];
            piece.faces = build.faces;
            m_OnPlane[side].assign(piece.vertices.size(), 0);
            for (size_t i = 0; i < globalOf.size(); ++i)
                m_OnPlane[side][i] = m_Sign[globalOf[i]] == 0 ? 1 : 0;

            // Edge kinds are rebuilt from this piece's adjacency (design 1.7).
            MeshEdgeUsageMap usage;
            CollectEdgeUsage(piece, usage);
            std::vector<uint64_t> keys;
            keys.reserve(usage.size());
            for (const auto& entry : usage)
                keys.push_back(entry.first);
            std::sort(keys.begin(), keys.end());
            for (uint64_t key : keys)
            {
                const MeshEdgeUsage& use = usage[key];
                const uint32_t ga = globalOf[EdgeKeyLow(key)], gb = globalOf[EdgeKeyHigh(key)];
                MeshEdgeTag inherited = MeshEdgeTag::Interior;
                const bool has = InheritedKind(ga, gb, inherited);
                if (use.count == 1)
                {
                    if (has && inherited != MeshEdgeTag::CapSeam)
                        piece.edgeTags[key] = inherited;
                    else
                    {
                        // A new boundary can only lie in the plane; anything else is a bug here.
                        if (m_Sign[ga] != 0 || m_Sign[gb] != 0)
                            return Fail(SliceStatus::InvalidInput, "internal: new boundary edge (" + Num(ga) + ", "
                                + Num(gb) + ") is off the plane");
                        piece.edgeTags[key] = MeshEdgeTag::Cut;
                    }
                }
                else if (use.count == 2 && has && inherited == MeshEdgeTag::CapSeam)
                    piece.edgeTags[key] = MeshEdgeTag::CapSeam;
            }

            // Cap records follow their cap faces.
            for (const MeshCutRecord& cap : m_In.caps)
            {
                bool present = false;
                for (const MeshFace& face : piece.faces)
                    if (face.kind == MeshFaceKind::Cap && face.cutId == cap.cutId)
                    {
                        present = true;
                        break;
                    }
                if (!present)
                    continue;
                MeshCutRecord moved = cap;
                for (uint32_t& rim : moved.rimVertices)
                {
                    if (rim >= remap.size() || remap[rim] == kInvalidIndex)
                        return Fail(SliceStatus::InvalidInput, "internal: rim vertex " + Num(rim) + " of cut " + Num(cap.cutId) + " was dropped");
                    rim = remap[rim];
                }
                piece.caps.push_back(moved);
            }
            return true;
        }

        SliceStatus Slicer::Run(GeometrySliceResult& out)
        {
            std::string why;
            SliceStatus status = ValidatePlane(m_Plane, &why);
            if (status == SliceStatus::Ok)
                status = ValidateMeshGeometry(m_In, m_Options.epsilonScale, &why);
            if (status != SliceStatus::Ok)
            {
                out.status = status;
                out.detail = why;
                return status;
            }
            MakePlaneBasis(m_Plane, m_Basis);
            m_Epsilon = ComputePieceRadius(m_In) * double(m_Options.epsilonScale);
            ClassifyVertices();

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

            MeshGeometry pieces[2];
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
                const SliceStatus outputStatus = ValidateMeshGeometry(pieces[side], m_Options.epsilonScale, &outputWhy);
                if (outputStatus != SliceStatus::Ok)
                {
                    out.status = outputStatus;
                    out.detail = std::string("output ") + (side == 0 ? "positive" : "negative") + " piece: " + outputWhy;
                    return outputStatus;
                }
            }

            // Cut loops come from each half's own boundary and must agree between the halves.
            std::vector<SliceLoop> loops[2];
            std::vector<std::vector<uint32_t>> chains[2];       // cap 없는 절단 경계 (GeometrySliceResult::uncapped 형식)
            for (int side = 0; side < 2; ++side)
            {
                std::string loopWhy;
                const SliceStatus loopStatus = ExtractCutLoops(pieces[side], m_OnPlane[side], m_Plane, side == 0,
                    m_Options.epsilonScale, loops[side], &loopWhy, &chains[side]);
                if (loopStatus != SliceStatus::Ok)
                {
                    out.status = loopStatus;
                    out.detail = std::string(side == 0 ? "positive" : "negative") + " piece: " + loopWhy;
                    return loopStatus;
                }
            }
            // 두 조각의 절단 에지 수는 같아야 한다. cap을 만들지 말지는 조각마다 다를 수 있다: 근접 허용오차가 조각 크기에
            // 비례해서, 같은 루프가 큰 조각에서는 빠지고 작은 조각에서는 남을 수 있다 (그 경우 한쪽만 cap이 생긴다).
            size_t cutEdges[2] = { 0, 0 };
            for (int side = 0; side < 2; ++side)
            {
                for (const SliceLoop& loop : loops[side])
                    cutEdges[side] += loop.vertices.size();
                for (const std::vector<uint32_t>& chain : chains[side])
                    cutEdges[side] += chain.empty() ? 0 : chain.size() - 1;
            }
            if (cutEdges[0] != cutEdges[1])
            {
                out.status = SliceStatus::InvalidLoop;
                out.detail = "internal: the two pieces disagree on the cut edges (" + Num(cutEdges[0]) + " vs "
                    + Num(cutEdges[1]) + ")";
                return out.status;
            }

            if (m_Options.cap == CapMode::Create)
            {
                for (int side = 0; side < 2; ++side)
                {
                    CapWhatFits(pieces[side], side == 0, loops[side], chains[side]);
                    // cap이 없는 경계는 이제 구멍의 일부다. Cut으로 남기면 다음 절단이 막힌다
                    for (const std::vector<uint32_t>& chain : chains[side])
                        for (size_t i = 0; i + 1 < chain.size(); ++i)
                        {
                            const auto tag = pieces[side].edgeTags.find(MakeEdgeKey(chain[i], chain[i + 1]));
                            if (tag != pieces[side].edgeTags.end() && tag->second == MeshEdgeTag::Cut)
                                tag->second = MeshEdgeTag::OpenSurface;
                        }
                }
            }

            out.positive = std::move(pieces[0]);
            out.negative = std::move(pieces[1]);
            for (const SliceLoop& loop : loops[0])
            {
                out.loops.push_back(loop.vertices);
                out.loopDepth.push_back(loop.depth);
                out.loopArea.push_back(loop.signedArea);
            }
            out.uncapped = std::move(chains[0]);
            out.status = SliceStatus::Ok;
            out.detail.clear();
            return SliceStatus::Ok;
        }
    }

    SliceStatus SliceGeometry(const MeshGeometry& in, const SlicePlane& plane, uint32_t cutId,
        const SliceOptions& options, GeometrySliceResult& out)
    {
        out = GeometrySliceResult();
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
            out.uncapped.clear();
        }
        return status;
    }

    bool SliceProceduralMesh(ProceduralMesh& inOutMesh, const DirectX::XMFLOAT3& planePosition,
        const DirectX::XMFLOAT3& planeNormal, bool createOtherHalf, ProceduralMesh* outOtherHalf,
        SliceCapOption capOption, uint32_t capMaterialSlot, SliceStatus* outStatus,
        std::string* error, std::vector<std::vector<uint32_t>>* outLoops)
    {
        if (outOtherHalf == &inOutMesh)
        {
            if (outStatus) *outStatus = SliceStatus::InvalidInput;
            if (error) *error = "The other half must be a different mesh";
            return false;
        }
        const MeshGeometry& geometry = inOutMesh.GetGeometry();
        // 새 절단 번호: 이미 쓴 가장 큰 cutId 다음
        uint32_t cutId = 0;
        for (const MeshVertex& vertex : geometry.vertices)
            cutId = std::max(cutId, vertex.cutId);
        for (const MeshFace& face : geometry.faces)
            cutId = std::max(cutId, face.cutId);
        for (const MeshCutRecord& cap : geometry.caps)
            cutId = std::max(cutId, cap.cutId);
        ++cutId;

        SliceOptions options;
        options.cap = capOption == SliceCapOption::NoCap ? CapMode::None : CapMode::Create;
        GeometrySliceResult result;
        const SliceStatus status = SliceGeometry(geometry, { planePosition, planeNormal }, cutId, options, result);
        if (outStatus)
            *outStatus = status;
        if (status != SliceStatus::Ok)
        {
            if (error)
                *error = result.detail;
            return false;
        }

        // 이번 절단의 cap 기록에 재질 규칙을 적는다 (BuildRenderMesh가 읽는다)
        for (MeshGeometry* piece : { &result.positive, &result.negative })
            for (MeshCutRecord& cap : piece->caps)
                if (cap.cutId == cutId)
                {
                    cap.materialSlot = capMaterialSlot;
                    cap.inLastSection = capOption == SliceCapOption::UseLastSectionForCap;
                }

        ProceduralMesh positive, negative;
        if (!positive.SetTopology(std::move(result.positive), error)
            || (createOtherHalf && outOtherHalf && !negative.SetTopology(std::move(result.negative), error)))
        {
            if (outStatus) *outStatus = SliceStatus::InvalidInput;
            return false;
        }
        inOutMesh = std::move(positive);
        if (outOtherHalf)
        {
            if (createOtherHalf)
                *outOtherHalf = std::move(negative);
            else
                outOtherHalf->Clear();
        }
        if (outLoops)
        {
            *outLoops = std::move(result.loops);
            for (std::vector<uint32_t>& chain : result.uncapped)
                outLoops->push_back(std::move(chain));
        }
        if (error)
            error->clear();
        return true;
    }
}
