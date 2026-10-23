// Standalone tests for the slicing pipeline (Geometry/) and the pure part of the plane gizmo
// (Geometry/CutPlaneGizmo.cpp). No D3D12 device is needed.
//
// Build and run from a Visual Studio developer prompt in MiniEngine/MeshCutting:
//   cl /nologo /utf-8 /EHsc /std:c++14 /W4 /MDd /Fo<outdir>\ /Fe<outdir>\PlaneSlicerTests.exe
//      tests\PlaneSlicerTests.cpp Geometry\*.cpp Rendering\RenderBake.cpp ProceduralMesh.cpp
//   <outdir>\PlaneSlicerTests.exe
// Keep <outdir> outside the repository. The exit code is the number of failed checks.

#include "../Geometry/CutPlane.h"
#include "../Geometry/CutPick.h"
#include "../Geometry/MeshGeometry.h"
#include "../Geometry/Skinning.h"
#include "../Geometry/Slice.h"
#include "../Rendering/RenderBake.h"
#include "../Runtime/CutJob.h"
#include "../ProceduralMesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace MeshCutting;
using namespace DirectX;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond) \
    do { \
        ++g_checks; \
        if (!(cond)) { ++g_failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

// `actual` runs exactly once (it usually fills `detail`).
#define CHECK_STATUS(actual, expected, detail) \
    do { \
        ++g_checks; \
        const SliceStatus checkedStatus = (actual); \
        if (checkedStatus != (expected)) { \
            ++g_failures; \
            std::printf("FAIL %s:%d  status %s, expected %s  (%s)\n", __FILE__, __LINE__, \
                SliceStatusName(checkedStatus), SliceStatusName(expected), (detail).c_str()); \
        } \
    } while (0)

// ---------------------------------------------------------------- builders

typedef std::array<uint32_t, 3> Tri;

// Flips the triangle when its normal points toward the center, so solids come out outward-facing.
static Tri OutwardTri(const std::vector<XMFLOAT3>& p, Tri t, const XMFLOAT3& center)
{
    const XMFLOAT3 &a = p[t[0]], &b = p[t[1]], &c = p[t[2]];
    const double e1[3] = { double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z };
    const double e2[3] = { double(c.x) - a.x, double(c.y) - a.y, double(c.z) - a.z };
    const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    const double m[3] = { (double(a.x) + b.x + c.x) / 3.0 - center.x, (double(a.y) + b.y + c.y) / 3.0 - center.y,
        (double(a.z) + b.z + c.z) / 3.0 - center.z };
    if (n[0] * m[0] + n[1] * m[1] + n[2] * m[2] < 0.0)
        std::swap(t[1], t[2]);
    return t;
}

// piece의 스킨 정점 표를 바꾼다 (Skinning은 읽기 전용이라 새로 만들어 끼운다).
static void SetSkinVertices(MeshGeometry& piece, std::vector<SkinVertex> vertices)
{
    piece.skin = std::make_shared<const Skinning>(std::move(vertices));
}

static std::vector<SkinVertex> GetSkinVertices(const MeshGeometry& piece)
{
    return piece.skin ? piece.skin->GetVertices() : std::vector<SkinVertex>();
}

// One vertex and one render corner per position, identity skin.
static MeshGeometry MakePiece(const std::vector<XMFLOAT3>& positions, const std::vector<Tri>& tris)
{
    MeshGeometry piece;
    std::vector<SkinVertex> skinVertices;
    for (size_t i = 0; i < positions.size(); ++i)
    {
        SkinVertex skin;
        skin.bindPosition = positions[i];
        skin.jointCount = 1;
        skin.joints[0] = 0;
        skin.weights[0] = 1.0f;
        skinVertices.push_back(skin);

        MeshVertex vertex;
        vertex.origin.skinVertex[0] = uint32_t(i);
        vertex.origin.bary[0] = 1.0f;
        vertex.position = positions[i];
        piece.vertices.push_back(vertex);

        MeshCorner corner;
        corner.vertex = uint32_t(i);
        corner.uv = { positions[i].x, positions[i].z };
        piece.corners.push_back(corner);
    }
    SetSkinVertices(piece, skinVertices);
    for (size_t i = 0; i < tris.size(); ++i)
    {
        MeshFace face;
        for (int k = 0; k < 3; ++k)
            face.corner[k] = tris[i][k];
        face.sourceTriangle = uint32_t(i);
        piece.faces.push_back(face);
    }
    return piece;
}

// Tags every boundary edge of a hand-built piece the way the weld step does for open patches.
static void TagOpenEdges(MeshGeometry& piece, MeshEdgeTag kind)
{
    MeshEdgeUsageMap usage;
    CollectEdgeUsage(piece, usage);
    for (const auto& entry : usage)
        if (entry.second.count == 1)
            piece.edgeTags[entry.first] = kind;
}

static MeshGeometry MakeBox()
{
    const std::vector<XMFLOAT3> p = {
        { -1, 0, -1 }, { 1, 0, -1 }, { 1, 0, 1 }, { -1, 0, 1 },
        { -1, 2, -1 }, { 1, 2, -1 }, { 1, 2, 1 }, { -1, 2, 1 } };
    // Side diagonals are 0-5, 1-6, 2-7 and 3-4.
    const std::vector<Tri> t = {
        { 0, 1, 2 }, { 0, 2, 3 }, { 4, 6, 5 }, { 4, 7, 6 },
        { 0, 4, 5 }, { 0, 5, 1 }, { 1, 5, 6 }, { 1, 6, 2 },
        { 2, 6, 7 }, { 2, 7, 3 }, { 3, 7, 4 }, { 3, 4, 0 } };
    return MakePiece(p, t);
}

// Tube around the Y axis with columns 0..n (column n repeats column 0 for the UV seam).
// seamJoint is the joint of the duplicated seam column.
static ProceduralMeshData MakeTubeMeshData(int n, bool seamDuplicate, uint16_t seamJoint)
{
    const double kTwoPi = 6.283185307179586;
    ProceduralMeshData in;
    const int columns = seamDuplicate ? n + 1 : n;
    for (int ring = 0; ring < 2; ++ring)
    {
        for (int c = 0; c < columns; ++c)
        {
            const bool dup = seamDuplicate && c == n;
            const double angle = kTwoPi * double(dup ? 0 : c) / double(n);
            MeshVertexBinding binding;
            MeshRenderVertex v = {};
            v.normal = { 0.0f, 1.0f, 0.0f };
            v.color = { 1.0f, 1.0f, 1.0f, 1.0f };
            binding.bindPosition = { float(std::cos(angle)), float(ring * 2), float(std::sin(angle)) };
            v.position = binding.bindPosition;
            v.uv = { float(c) / float(n), float(ring) };
            binding.joints[0] = dup ? seamJoint : 0;
            binding.weights[0] = 1.0f;
            in.vertices.push_back(v);
            in.bindings.push_back(binding);
        }
    }
    const auto add = [&](uint32_t a, uint32_t b, uint32_t c)
    {
        const XMFLOAT3 &pa = in.vertices[a].position, &pb = in.vertices[b].position, &pc = in.vertices[c].position;
        const double e1[3] = { double(pb.x) - pa.x, double(pb.y) - pa.y, double(pb.z) - pa.z };
        const double e2[3] = { double(pc.x) - pa.x, double(pc.y) - pa.y, double(pc.z) - pa.z };
        const double nx = e1[1] * e2[2] - e1[2] * e2[1], nz = e1[0] * e2[1] - e1[1] * e2[0];
        const double cx = (double(pa.x) + pb.x + pc.x) / 3.0, cz = (double(pa.z) + pb.z + pc.z) / 3.0;
        if (nx * cx + nz * cz < 0.0)
            std::swap(b, c);
        in.indices.insert(in.indices.end(), { a, b, c });
    };
    for (int c = 0; c < n; ++c)
    {
        const uint32_t b0 = uint32_t(c), b1 = uint32_t((c + 1) % columns);
        const uint32_t t0 = uint32_t(columns + c), t1 = uint32_t(columns + (c + 1) % columns);
        add(b0, b1, t1);
        add(b0, t1, t0);
    }
    return in;
}

static ProceduralMeshData MakeBoxMeshData()
{
    const MeshGeometry box = MakeBox();
    ProceduralMeshData in;
    for (size_t i = 0; i < box.vertices.size(); ++i)
    {
        MeshRenderVertex v = {};
        v.position = box.vertices[i].position;
        v.normal = { 0.0f, 1.0f, 0.0f };
        v.color = { 1.0f, 1.0f, 1.0f, 1.0f };
        MeshVertexBinding binding;
        binding.bindPosition = v.position;
        binding.weights[0] = 1.0f;
        in.vertices.push_back(v);
        in.bindings.push_back(binding);
    }
    for (const MeshFace& f : box.faces)
        in.indices.insert(in.indices.end(), { f.corner[0], f.corner[1], f.corner[2] });
    return in;
}

static uint32_t CountKind(const MeshGeometry& piece, MeshEdgeTag kind)
{
    uint32_t n = 0;
    for (const auto& entry : piece.edgeTags)
        if (entry.second == kind)
            ++n;
    return n;
}

// ---------------------------------------------------------------- 2-1 tests

static void TestWeldBox()
{
    MeshGeometry piece;
    WeldStats stats;
    std::string detail;
    const SliceStatus status = BuildMeshGeometry(MakeBoxMeshData(), kDefaultEpsilonScale, piece, stats, &detail);
    CHECK_STATUS(status, SliceStatus::Ok, detail);
    CHECK(stats.corners == 8 && stats.vertices == 8 && stats.faces == 12);
    CHECK(stats.boundaryEdges == 0 && piece.edgeTags.empty());
    CHECK(piece.skin->GetVertices().size() == 8 && piece.vertices[3].origin.skinVertex[0] == 3 && piece.vertices[3].origin.bary[0] == 1.0f);
    CHECK(piece.faces[5].sourceTriangle == 5);
}

static void TestWeldUvSeamTube()
{
    // Same position and skin on both sides of the UV seam: welded into one vertex per column.
    MeshGeometry piece;
    WeldStats stats;
    std::string detail;
    const SliceStatus status = BuildMeshGeometry(MakeTubeMeshData(8, true, 0), kDefaultEpsilonScale, piece, stats, &detail);
    CHECK_STATUS(status, SliceStatus::Ok, detail);
    CHECK(stats.corners == 18 && stats.vertices == 16);
    CHECK(stats.positionVertices == 16 && stats.skinSplitPositions == 0);
    CHECK(stats.unweldedSeamEdges == 0);
    CHECK(stats.replacementEdges == 16);        // 8 bottom + 8 top rim edges
    CHECK(CountKind(piece, MeshEdgeTag::Replacement) == 16);
    // The two seam corners keep their own UVs but share one vertex (and so one position).
    CHECK(piece.corners[0].vertex == piece.corners[8].vertex);
    CHECK(piece.corners[0].uv.x == 0.0f && piece.corners[8].uv.x == 1.0f);
}

static void TestWeldSkinSplitSeam()
{
    // Seam duplicates with a different joint are not welded; the seam stays open and is tagged.
    MeshGeometry piece;
    WeldStats stats;
    std::string detail;
    const SliceStatus status = BuildMeshGeometry(MakeTubeMeshData(8, true, 1), kDefaultEpsilonScale, piece, stats, &detail);
    CHECK_STATUS(status, SliceStatus::Ok, detail);
    CHECK(stats.corners == 18 && stats.vertices == 18);
    CHECK(stats.positionVertices == 16 && stats.skinSplitPositions == 2);
    CHECK(stats.unweldedSeamEdges == 2);        // the vertical edge on each side of the seam
    CHECK(CountKind(piece, MeshEdgeTag::UnweldedSeam) == 2);
    CHECK(stats.replacementEdges == 16);
}

static void TestWeldNegativeZero()
{
    ProceduralMeshData in = MakeBoxMeshData();
    for (MeshVertexBinding& v : in.bindings)
        if (v.bindPosition.x == -1.0f && v.bindPosition.y == 0.0f && v.bindPosition.z == -1.0f)
            v.bindPosition.y = -0.0f;
    in.vertices.push_back(in.vertices[0]);           // duplicate of vertex 0
    in.bindings.push_back(in.bindings[0]);
    in.bindings.back().bindPosition.y = 0.0f;        // +0 against -0
    in.indices.insert(in.indices.end(), { 8, 1, 2 });  // makes the duplicate used (shares edge 1-2 reversed)
    MeshGeometry piece;
    WeldStats stats;
    std::string detail;
    BuildMeshGeometry(in, kDefaultEpsilonScale, piece, stats, &detail);
    CHECK(stats.corners == 9 && stats.vertices == 8);   // -0 and +0 weld together
}

static void TestInputRejects()
{
    MeshGeometry piece;
    WeldStats stats;
    std::string detail;

    ProceduralMeshData nanPosition = MakeBoxMeshData();
    nanPosition.vertices[2].position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_STATUS(BuildMeshGeometry(nanPosition, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::InvalidInput, detail);
    CHECK(piece.Empty());

    ProceduralMeshData badIndex = MakeBoxMeshData();
    badIndex.indices[4] = 99;
    CHECK_STATUS(BuildMeshGeometry(badIndex, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::InvalidInput, detail);

    ProceduralMeshData noWeight = MakeBoxMeshData();
    noWeight.bindings[1].weights[0] = 0.0f;
    CHECK_STATUS(BuildMeshGeometry(noWeight, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::InvalidInput, detail);

    ProceduralMeshData badCount = MakeBoxMeshData();
    badCount.indices.pop_back();
    CHECK_STATUS(BuildMeshGeometry(badCount, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::InvalidInput, detail);

    ProceduralMeshData missingBindings = MakeBoxMeshData();
    missingBindings.bindings.clear();
    CHECK_STATUS(BuildMeshGeometry(missingBindings, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::InvalidInput, detail);
    CHECK(piece.Empty());

    ProceduralMeshData shortBindings = MakeBoxMeshData();
    shortBindings.bindings.pop_back();
    CHECK_STATUS(BuildMeshGeometry(shortBindings, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::InvalidInput, detail);

    // Hand-edited pieces for the structural checks.
    const MeshGeometry good = MakeBox();
    CHECK_STATUS(ValidateMeshGeometry(good, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);

    MeshGeometry badOrigin = good;
    badOrigin.vertices[3].origin.bary[0] = 0.5f;
    CHECK_STATUS(ValidateMeshGeometry(badOrigin, kDefaultEpsilonScale, &detail), SliceStatus::InvalidOrigin, detail);

    MeshGeometry badSkinVertex = good;
    badSkinVertex.vertices[3].origin.skinVertex[0] = 99;
    CHECK_STATUS(ValidateMeshGeometry(badSkinVertex, kDefaultEpsilonScale, &detail), SliceStatus::InvalidOrigin, detail);

    MeshGeometry unsortedOrigin = good;
    unsortedOrigin.vertices[3].origin.skinVertex[0] = 5;
    unsortedOrigin.vertices[3].origin.skinVertex[1] = 3;
    unsortedOrigin.vertices[3].origin.bary[0] = 0.5f;
    unsortedOrigin.vertices[3].origin.bary[1] = 0.5f;
    CHECK_STATUS(ValidateMeshGeometry(unsortedOrigin, kDefaultEpsilonScale, &detail), SliceStatus::InvalidOrigin, detail);

    MeshGeometry badCorner = good;
    badCorner.corners[2].vertex = 99;
    CHECK_STATUS(ValidateMeshGeometry(badCorner, kDefaultEpsilonScale, &detail), SliceStatus::InvalidInput, detail);

    MeshGeometry badFace = good;
    badFace.faces[0].corner[1] = 99;
    CHECK_STATUS(ValidateMeshGeometry(badFace, kDefaultEpsilonScale, &detail), SliceStatus::InvalidInput, detail);

    MeshGeometry repeated = good;
    repeated.faces[0].corner[1] = repeated.faces[0].corner[0];
    CHECK_STATUS(ValidateMeshGeometry(repeated, kDefaultEpsilonScale, &detail), SliceStatus::DegenerateInput, detail);

    MeshGeometry flat = good;
    flat.vertices[1].position = flat.vertices[0].position;   // collapses faces that use the edge 0-1
    flat.vertices[1].position.x += 1e-9f;
    CHECK_STATUS(ValidateMeshGeometry(flat, kDefaultEpsilonScale, &detail), SliceStatus::DegenerateInput, detail);

    // Three faces on one edge.
    MeshGeometry fin = good;
    fin.vertices.push_back(fin.vertices[0]);
    fin.vertices.back().position = { 0.0f, 1.0f, -3.0f };
    fin.vertices.back().origin.skinVertex[0] = 8;
    std::vector<SkinVertex> finSkin = GetSkinVertices(fin);
    finSkin.push_back(finSkin[0]);
    SetSkinVertices(fin, finSkin);
    MeshCorner extra;
    extra.vertex = 8;
    fin.corners.push_back(extra);
    MeshFace fan;
    fan.corner[0] = 0; fan.corner[1] = 1; fan.corner[2] = 8;   // edge 0-1 already has two faces
    fin.faces.push_back(fan);
    CHECK_STATUS(ValidateMeshGeometry(fin, kDefaultEpsilonScale, &detail), SliceStatus::NonManifoldInput, detail);

    // Two faces use an interior edge in the same direction (one face flipped).
    MeshGeometry flipped = good;
    std::swap(flipped.faces[0].corner[1], flipped.faces[0].corner[2]);
    CHECK_STATUS(ValidateMeshGeometry(flipped, kDefaultEpsilonScale, &detail), SliceStatus::NonManifoldInput, detail);

    // A non-finite or negative epsilon scale is rejected as well.
    CHECK_STATUS(ValidateMeshGeometry(good, -1.0f, &detail), SliceStatus::InvalidInput, detail);
}

// An open edge without a kind (or tagged Interior/CapSeam) is rejected up front.
static void TestUntaggedBoundaryIsRejected()
{
    MeshGeometry sheet = MakePiece({ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } }, { { 0, 1, 2 } });
    std::string detail;
    CHECK_STATUS(ValidateMeshGeometry(sheet, kDefaultEpsilonScale, &detail), SliceStatus::InvalidInput, detail);
    TagOpenEdges(sheet, MeshEdgeTag::Replacement);
    CHECK_STATUS(ValidateMeshGeometry(sheet, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    sheet.edgeTags[MakeEdgeKey(0, 1)] = MeshEdgeTag::CapSeam;
    CHECK_STATUS(ValidateMeshGeometry(sheet, kDefaultEpsilonScale, &detail), SliceStatus::InvalidInput, detail);
}

static void TestValidatePlane()
{
    std::string detail;
    SlicePlane plane;
    CHECK_STATUS(ValidatePlane(plane, &detail), SliceStatus::Ok, detail);
    plane.normal = { 0.0f, 0.0f, 0.0f };
    CHECK_STATUS(ValidatePlane(plane, &detail), SliceStatus::InvalidInput, detail);
    plane.normal = { 0.0f, 1.0f, 0.0f };
    plane.point.y = std::numeric_limits<float>::infinity();
    CHECK_STATUS(ValidatePlane(plane, &detail), SliceStatus::InvalidInput, detail);
}

static void TestOpenGridBoundaryKinds()
{
    // A flat 3x3 sheet is entirely open boundary. Hand-built pieces tag it as Replacement.
    std::vector<XMFLOAT3> p;
    for (int y = 0; y <= 3; ++y)
        for (int x = 0; x <= 3; ++x)
            p.push_back({ float(x), float(y), 0.0f });
    std::vector<Tri> t;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
        {
            const uint32_t a = uint32_t(y * 4 + x);
            t.push_back({ a, a + 1, a + 5 });
            t.push_back({ a, a + 5, a + 4 });
        }
    MeshGeometry grid = MakePiece(p, t);
    TagOpenEdges(grid, MeshEdgeTag::Replacement);
    std::string detail;
    CHECK_STATUS(ValidateMeshGeometry(grid, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    CHECK(CountKind(grid, MeshEdgeTag::Replacement) == 12);   // perimeter of a 3x3 sheet
}

static void TestBoundaryDebugBake()
{
    MeshGeometry piece;
    WeldStats stats;
    std::string detail;
    BuildMeshGeometry(MakeTubeMeshData(8, true, 1), kDefaultEpsilonScale, piece, stats, &detail);
    ProceduralMeshData data;
    BakeBoundaryDebug(piece, 0.02f, data);
    CHECK(data.vertices.size() == 6u * 18u);           // 16 rim + 2 seam edges, 6 vertices each
    CHECK(data.indices.size() == 18u * 18u);
    ProceduralMesh mesh;
    std::string error;
    CHECK(mesh.SetMesh(data, &error));
}

// ---------------------------------------------------------------- 2-2 tests

static SlicePlane MakePlane(float px, float py, float pz, float nx, float ny, float nz)
{
    SlicePlane plane;
    plane.point = { px, py, pz };
    plane.normal = { nx, ny, nz };
    return plane;
}

static SliceOptions NoCapOptions()
{
    SliceOptions options;
    options.cap = CapMode::None;
    return options;
}

static uint32_t CountCutNodes(const MeshGeometry& piece, uint32_t cutId)
{
    uint32_t n = 0;
    for (const MeshVertex& vertex : piece.vertices)
        if (vertex.cutId == cutId)
            ++n;
    return n;
}

static double TotalSurfaceArea(const MeshGeometry& piece) { return TotalArea(piece, MeshFaceKind::Surface); }

// Both output pieces must pass the same validation as an input piece.
static void CheckPieceValid(const MeshGeometry& piece, const char* label)
{
    std::string detail;
    const SliceStatus status = ValidateMeshGeometry(piece, kDefaultEpsilonScale, &detail);
    ++g_checks;
    if (status != SliceStatus::Ok)
    {
        ++g_failures;
        std::printf("FAIL %s: output piece is invalid: %s (%s)\n", label, SliceStatusName(status), detail.c_str());
    }
}

static MeshGeometry MakeOctahedron()
{
    const std::vector<XMFLOAT3> p = {
        { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, -1, 0 } };
    std::vector<Tri> t;
    for (uint32_t i = 0; i < 4; ++i)
    {
        t.push_back(OutwardTri(p, { 4, i, (i + 1) % 4 }, { 0, 0, 0 }));
        t.push_back(OutwardTri(p, { 5, i, (i + 1) % 4 }, { 0, 0, 0 }));
    }
    return MakePiece(p, t);
}

// Case 1: closed box, every side face split by a diagonal, horizontal cut through the middle.
// The plane crosses 4 vertical edges and the 4 diagonals (0-5, 1-6, 2-7, 3-4) = 8 cut vertices.
static void TestBoxCenterCut()
{
    const MeshGeometry box = MakeBox();
    GeometrySliceResult result;
    const SliceStatus status = SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result);
    CHECK_STATUS(status, SliceStatus::Ok, result.detail);
    CHECK(result.positive.vertices.size() == 12 && result.negative.vertices.size() == 12);   // 4 box vertices + 8 cut vertices
    CHECK(CountCutNodes(result.positive, 1) == 8 && CountCutNodes(result.negative, 1) == 8);
    CHECK(CountKind(result.positive, MeshEdgeTag::Cut) == 8 && CountKind(result.negative, MeshEdgeTag::Cut) == 8);
    CHECK(result.positive.faces.size() + result.negative.faces.size() > box.faces.size());
    CheckPieceValid(result.positive, "box +");
    CheckPieceValid(result.negative, "box -");

    // The two pieces cover exactly the original surface.
    const double original = TotalSurfaceArea(box);
    const double sum = TotalSurfaceArea(result.positive) + TotalSurfaceArea(result.negative);
    CHECK(std::fabs(sum - original) < original * 1e-5);

    // Cut vertices lie on the plane and are shared by both pieces (same position bits).
    for (const MeshVertex& vertex : result.positive.vertices)
        if (vertex.cutId == 1)
            CHECK(std::fabs(vertex.position.y - 1.0f) < 1e-6f);
    for (const MeshVertex& a : result.positive.vertices)
    {
        if (a.cutId != 1)
            continue;
        bool matched = false;
        for (const MeshVertex& b : result.negative.vertices)
            matched = matched || (b.cutId == 1 && b.position.x == a.position.x && b.position.y == a.position.y
                && b.position.z == a.position.z);
        CHECK(matched);
    }
    // Positive side keeps the top: its original vertices are the four at y = 2.
    uint32_t top = 0;
    for (const MeshVertex& vertex : result.positive.vertices)
        if (vertex.cutId == 0)
        {
            ++top;
            CHECK(vertex.position.y == 2.0f);
        }
    CHECK(top == 4);
    // One outer loop of 8 vertices around a 2 x 2 square, counter-clockwise in the plane basis for
    // the positive piece.
    CHECK(result.loops.size() == 1 && result.loops[0].size() == 8);
    CHECK(result.loopDepth.size() == 1 && result.loopDepth[0] == 0);
    CHECK(result.loopArea.size() == 1 && std::fabs(result.loopArea[0] - 4.0) < 1e-5);
    // The input is untouched.
    CHECK(box.vertices.size() == 8 && box.faces.size() == 12);
}

// Case 2: the plane passes through vertices (the octahedron equator). No new vertices, no zero-area
// faces, and the four equator edges become the boundary of both pieces.
static void TestPlaneThroughNodes()
{
    const MeshGeometry octa = MakeOctahedron();
    GeometrySliceResult result;
    const SliceStatus status = SliceGeometry(octa, MakePlane(0, 0, 0, 0, 1, 0), 1, NoCapOptions(), result);
    CHECK_STATUS(status, SliceStatus::Ok, result.detail);
    CHECK(result.positive.vertices.size() == 5 && result.negative.vertices.size() == 5);
    CHECK(CountCutNodes(result.positive, 1) == 0 && CountCutNodes(result.negative, 1) == 0);
    CHECK(result.positive.faces.size() == 4 && result.negative.faces.size() == 4);
    CHECK(CountKind(result.positive, MeshEdgeTag::Cut) == 4 && CountKind(result.negative, MeshEdgeTag::Cut) == 4);
    CheckPieceValid(result.positive, "octahedron +");
    CheckPieceValid(result.negative, "octahedron -");
    // The loop is the equator diamond: 4 existing vertices, area 2.
    CHECK(result.loops.size() == 1 && result.loops[0].size() == 4);
    CHECK(result.loopArea.size() == 1 && std::fabs(result.loopArea[0] - 2.0) < 1e-6);
}

// A plane through exactly one vertex: the faces around it split the opposite edge ((0, +, -) faces)
// or keep a lone corner ((+, -, -) faces), and the cut loop passes through the existing vertex.
static void TestOneNodeOnPlane()
{
    const MeshGeometry octa = MakeOctahedron();
    GeometrySliceResult result;
    const SliceStatus status = SliceGeometry(octa, MakePlane(1, 0, 0, 1, 2, 0), 1, NoCapOptions(), result);
    CHECK_STATUS(status, SliceStatus::Ok, result.detail);
    CHECK(CountCutNodes(result.positive, 1) == 3);      // edges apex-e1, apex-e2, apex-e3
    CHECK(result.loops.size() == 1 && result.loops[0].size() == 4);     // those 3 + the vertex on the plane
    CHECK(CountKind(result.positive, MeshEdgeTag::Cut) == 4 && CountKind(result.negative, MeshEdgeTag::Cut) == 4);
    CheckPieceValid(result.positive, "one vertex +");
    CheckPieceValid(result.negative, "one vertex -");
    const double original = TotalSurfaceArea(octa);
    CHECK(std::fabs(TotalSurfaceArea(result.positive) + TotalSurfaceArea(result.negative) - original) < original * 1e-5);
}

// Case 6: an open piece. The cut chain runs from boundary to boundary and is reported as OpenLoop,
// naming the kind of boundary it ends on.
static void TestOpenSheetIsOpenLoop()
{
    std::vector<XMFLOAT3> p;
    for (int y = 0; y <= 3; ++y)
        for (int x = 0; x <= 3; ++x)
            p.push_back({ float(x), float(y), 0.0f });
    std::vector<Tri> t;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
        {
            const uint32_t a = uint32_t(y * 4 + x);
            t.push_back({ a, a + 1, a + 5 });
            t.push_back({ a, a + 5, a + 4 });
        }
    MeshGeometry grid = MakePiece(p, t);
    TagOpenEdges(grid, MeshEdgeTag::Replacement);
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(grid, MakePlane(1.5f, 0, 0, 1, 0, 0), 1, NoCapOptions(), result), SliceStatus::OpenLoop, result.detail);
    CHECK(result.detail.find("Replacement") != std::string::npos);
    CHECK(result.positive.Empty() && result.negative.Empty() && result.loops.empty());

    // A single triangle cut through one corner is open as well.
    MeshGeometry tri = MakePiece({ { 0, 0, 0 }, { 1, 1, 0 }, { 1, -1, 0 } }, { { 0, 1, 2 } });
    TagOpenEdges(tri, MeshEdgeTag::Replacement);
    CHECK_STATUS(SliceGeometry(tri, MakePlane(0, 0, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::OpenLoop, result.detail);
}

static uint32_t CountCapFaces(const MeshGeometry& piece);
static SliceOptions CapOptions();

// Unreal 방식: 원본의 구멍(OpenSurface)에서 끝나는 사슬은 실패가 아니라 cap 없는 열린 사슬이다.
// 그 에지는 OpenSurface가 되어, 같은 조각을 다시 잘라도 OpenLoop가 나지 않는다.
static void TestOpenSurfaceChainHasNoCap()
{
    std::vector<XMFLOAT3> p;
    for (int y = 0; y <= 3; ++y)
        for (int x = 0; x <= 3; ++x)
            p.push_back({ float(x), float(y), 0.0f });
    std::vector<Tri> t;
    for (int y = 0; y < 3; ++y)
        for (int x = 0; x < 3; ++x)
        {
            const uint32_t a = uint32_t(y * 4 + x);
            t.push_back({ a, a + 1, a + 5 });
            t.push_back({ a, a + 5, a + 4 });
        }
    MeshGeometry grid = MakePiece(p, t);
    TagOpenEdges(grid, MeshEdgeTag::OpenSurface);
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(grid, MakePlane(1.5f, 0, 0, 1, 0, 0), 1, SliceOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.empty());
    CHECK(result.uncapped.size() == 1);
    CHECK(!result.uncapped.empty() && result.uncapped[0].size() >= 2);
    for (const MeshGeometry* piece : { &result.positive, &result.negative })
    {
        CHECK(CountCapFaces(*piece) == 0);
        CHECK(CountKind(*piece, MeshEdgeTag::Cut) == 0);       // 열린 사슬 에지도 구멍(OpenSurface)으로 바뀐다
        CheckPieceValid(*piece, "open surface piece");
    }

    // 잘린 조각을 다시 잘라도 이전 사슬이 구멍으로 다뤄진다
    GeometrySliceResult again;
    const MeshGeometry left = result.negative;
    CHECK_STATUS(SliceGeometry(left, MakePlane(0, 1.5f, 0, 0, 1, 0), 2, SliceOptions(), again), SliceStatus::Ok, again.detail);
    CHECK(again.uncapped.size() == 1 && again.loops.empty());

    // NoCap은 기존 규칙대로 Cut 경계를 남긴다
    CHECK_STATUS(SliceGeometry(grid, MakePlane(1.5f, 0, 0, 1, 0, 0), 1, NoCapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(CountKind(result.positive, MeshEdgeTag::Cut) > 0);
}

// Case 3: an edge lies on the plane and both faces around it are on the same side (touching).
static void TestTouchingIsNoCut()
{
    const std::vector<XMFLOAT3> p = { { 0, 0, 0 }, { 1, 0, 0 }, { 0.5f, 1, 0.5f }, { 0.5f, 1, -0.5f } };
    const XMFLOAT3 center = { 0.5f, 0.5f, 0.0f };
    const std::vector<Tri> t = { OutwardTri(p, { 0, 1, 2 }, center), OutwardTri(p, { 0, 3, 1 }, center),
        OutwardTri(p, { 0, 2, 3 }, center), OutwardTri(p, { 1, 3, 2 }, center) };
    const MeshGeometry tet = MakePiece(p, t);
    GeometrySliceResult result;
    const SliceStatus status = SliceGeometry(tet, MakePlane(0, 0, 0, 0, 1, 0), 1, NoCapOptions(), result);
    CHECK_STATUS(status, SliceStatus::NoCut, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());
}

// Case 4: a face lies in the plane.
static void TestCoplanarFace()
{
    const std::vector<XMFLOAT3> p = { { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 1 }, { 0.3f, 1, 0.3f } };
    const XMFLOAT3 center = { 0.3f, 0.25f, 0.3f };
    const std::vector<Tri> t = { OutwardTri(p, { 0, 1, 2 }, center), OutwardTri(p, { 0, 3, 1 }, center),
        OutwardTri(p, { 1, 3, 2 }, center), OutwardTri(p, { 2, 3, 0 }, center) };
    const MeshGeometry tet = MakePiece(p, t);
    GeometrySliceResult result;
    const SliceStatus status = SliceGeometry(tet, MakePlane(0, 0, 0, 0, 1, 0), 1, NoCapOptions(), result);
    CHECK_STATUS(status, SliceStatus::CoplanarFace, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());
}

// Case 5: the plane misses the piece.
static void TestMissIsNoCut()
{
    const MeshGeometry box = MakeBox();
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 10, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::NoCut, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty() && result.loops.empty());
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, -1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::NoCut, result.detail);
}

// Case 12: failures leave nothing but status and detail, even when out held a previous result.
static void TestFailureClearsOut()
{
    const MeshGeometry box = MakeBox();
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(!result.positive.Empty() && !result.negative.Empty());

    result.loops.push_back({ 1, 2, 3 });
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 10, 0, 0, 1, 0), 2, NoCapOptions(), result), SliceStatus::NoCut, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty() && result.loops.empty());
    CHECK(result.positive.vertices.empty() && result.negative.corners.empty());
    CHECK(!result.detail.empty());

    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 0, 0), 1, NoCapOptions(), result), SliceStatus::InvalidInput, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());

    MeshGeometry broken = box;
    broken.faces[3].corner[0] = 99;
    CHECK_STATUS(SliceGeometry(broken, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::InvalidInput, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());
}

// Position bits come from the vertex: every corner of one vertex bakes to the same position.
static void TestBakeSharesNodePositions()
{
    MeshGeometry strip = MakePiece({ { 0, 0, 0 }, { 0, 2, 0 }, { 1, 1, 0 }, { -1, 1, 0 } }, { { 0, 1, 2 }, { 1, 0, 3 } });
    MeshCorner a2 = strip.corners[0];
    a2.uv = { 5.0f, 5.0f };
    strip.corners.push_back(a2);
    strip.faces[1].corner[1] = 4;
    ProceduralMeshData baked;
    BuildRenderMesh(strip, baked);
    CHECK(baked.vertices.size() == 5 && baked.indices.size() == 6 && baked.sections.size() == 1);
    CHECK(baked.vertices[0].position.x == baked.vertices[4].position.x && baked.vertices[0].position.y == baked.vertices[4].position.y
        && baked.vertices[0].position.z == baked.vertices[4].position.z);
    CHECK(baked.vertices[4].uv.x == 5.0f);

    // A cap face moves to its own section; empty sections are never created.
    strip.faces[1].kind = MeshFaceKind::Cap;
    BuildRenderMesh(strip, baked);
    CHECK(baked.sections.size() == 2 && baked.sections[0].indexCount == 3 && baked.sections[1].firstIndex == 3
        && baked.sections[1].indexCount == 3 && baked.sections[1].materialSlot == 1);
    ProceduralMesh mesh;
    std::string error;
    CHECK(mesh.SetMesh(baked, &error));
}

// A cut vertex may depend on at most three original vertices.
static void TestOriginLimit()
{
    MeshGeometry box = MakeBox();
    for (int i = 0; i < 3; ++i)
    {
        box.vertices[0].origin.skinVertex[i] = uint32_t(i);
        box.vertices[0].origin.bary[i] = 1.0f / 3.0f;
    }
    box.vertices[4].origin.skinVertex[0] = 3;
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::InvalidOrigin, result.detail);
    CHECK(result.positive.Empty());
}

// The plane may never split a cap face.
static void TestCapFaceIsNotCrossed()
{
    MeshGeometry box = MakeBox();
    box.faces[4].kind = MeshFaceKind::Cap;
    box.faces[4].cutId = 7;
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::CrossesCap, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());
}

// A tilted plane splits the box into two non-empty pieces whose areas still add up.
static void TestTiltedCutKeepsArea()
{
    const MeshGeometry box = MakeBox();
    GeometrySliceResult result;
    const SliceStatus status = SliceGeometry(box, MakePlane(0.1f, 0.9f, -0.2f, 0.3f, 1.0f, 0.2f), 1, NoCapOptions(), result);
    CHECK_STATUS(status, SliceStatus::Ok, result.detail);
    CheckPieceValid(result.positive, "tilted +");
    CheckPieceValid(result.negative, "tilted -");
    const double original = TotalSurfaceArea(box);
    CHECK(std::fabs(TotalSurfaceArea(result.positive) + TotalSurfaceArea(result.negative) - original) < original * 1e-5);
}

// A face just above the degeneracy threshold can clip into pieces below it. Such a result would
// not be a valid input for the next cut, so it is rejected instead of published.
static void TestSliverOutputIsRejected()
{
    // A large sheet sets the piece radius; the thin triangle sits far from it.
    std::vector<XMFLOAT3> p = { { 0, 0, 0 }, { 10, 0, 0 }, { 10, 10, 0 }, { 0, 10, 0 } };
    std::vector<Tri> t = { { 0, 1, 2 }, { 0, 2, 3 } };
    const float thin = 4e-8f;
    p.push_back({ 20, 0, 0 });
    p.push_back({ 21, 0, 0 });
    p.push_back({ 21, thin, 0 });
    t.push_back({ 4, 5, 6 });
    MeshGeometry piece = MakePiece(p, t);
    TagOpenEdges(piece, MeshEdgeTag::Replacement);
    std::string detail;
    CHECK_STATUS(ValidateMeshGeometry(piece, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);

    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(piece, MakePlane(20.5f, 0, 0, 1, 0, 0), 1, NoCapOptions(), result),
        SliceStatus::DegenerateInput, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());
    CHECK(result.detail.find("output") == 0);

    // The same cut on a triangle that is not marginal is fine.
    piece.vertices[6].position.y = 1e-3f;
    // The sheet is open, so the cut ends as OpenLoop: reaching the loop stage shows the output
    // pieces passed validation (a sliver would have stopped earlier with DegenerateInput).
    CHECK_STATUS(SliceGeometry(piece, MakePlane(20.5f, 0, 0, 1, 0, 0), 1, NoCapOptions(), result), SliceStatus::OpenLoop, result.detail);
}

// ---------------------------------------------------------------- 2-3 tests

// src를 dst 뒤에 붙인다 (정점, corner, 면, 스킨 정점, 에지 태그 번호를 다시 매긴다).
static void AppendPiece(MeshGeometry& dst, const MeshGeometry& src)
{
    const uint32_t nodeBase = uint32_t(dst.vertices.size());
    const uint32_t cornerBase = uint32_t(dst.corners.size());
    std::vector<SkinVertex> skinVertices = GetSkinVertices(dst);
    const uint32_t originBase = uint32_t(skinVertices.size());
    for (const SkinVertex& skin : GetSkinVertices(src))
        skinVertices.push_back(skin);
    SetSkinVertices(dst, skinVertices);
    for (MeshVertex vertex : src.vertices)
    {
        for (int i = 0; i < 3; ++i)
            if (vertex.origin.skinVertex[i] != kInvalidIndex)
                vertex.origin.skinVertex[i] += originBase;
        dst.vertices.push_back(vertex);
    }
    for (MeshCorner corner : src.corners)
    {
        corner.vertex += nodeBase;
        dst.corners.push_back(corner);
    }
    for (MeshFace face : src.faces)
    {
        for (int k = 0; k < 3; ++k)
            face.corner[k] += cornerBase;
        dst.faces.push_back(face);
    }
    for (const auto& entry : src.edgeTags)
        dst.edgeTags[MakeEdgeKey(EdgeKeyLow(entry.first) + nodeBase, EdgeKeyHigh(entry.first) + nodeBase)] = entry.second;
}

// The box scaled to a square of half-size `half` around (cx, cz) with the given top height.
static MeshGeometry MakeBoxAt(float cx, float cz, float half, float height)
{
    MeshGeometry box = MakeBox();
    for (MeshVertex& vertex : box.vertices)
    {
        vertex.position.x = cx + vertex.position.x * half;
        vertex.position.z = cz + vertex.position.z * half;
        vertex.position.y = vertex.position.y * 0.5f * height;
    }
    return box;
}

static Tri OrientToward(const std::vector<XMFLOAT3>& p, Tri t, double dx, double dy, double dz)
{
    const XMFLOAT3 &a = p[t[0]], &b = p[t[1]], &c = p[t[2]];
    const double e1[3] = { double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z };
    const double e2[3] = { double(c.x) - a.x, double(c.y) - a.y, double(c.z) - a.z };
    const double n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    if (n[0] * dx + n[1] * dy + n[2] * dz < 0.0)
        std::swap(t[1], t[2]);
    return t;
}

// Closed thick tube around the Y axis (outer radius R, inner radius r, height 2) with n columns.
static MeshGeometry MakeThickTube(int n, float outer, float inner)
{
    const double kTwoPi = 6.283185307179586;
    std::vector<XMFLOAT3> p;
    const float radius[4] = { outer, outer, inner, inner };
    const float height[4] = { 0.0f, 2.0f, 0.0f, 2.0f };      // outer bottom/top, inner bottom/top
    for (int ring = 0; ring < 4; ++ring)
        for (int i = 0; i < n; ++i)
        {
            const double a = kTwoPi * i / n;
            p.push_back({ float(radius[ring] * std::cos(a)), height[ring], float(radius[ring] * std::sin(a)) });
        }
    const auto id = [&](int ring, int i) { return uint32_t(ring * n + (i % n)); };
    std::vector<Tri> t;
    const auto addQuad = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, double sx, double sy, double sz)
    {
        const double cx = (double(p[a].x) + p[b].x + p[c].x) / 3.0, cz = (double(p[a].z) + p[b].z + p[c].z) / 3.0;
        t.push_back(OrientToward(p, { a, b, c }, sx * cx, sy, sz * cz));
        t.push_back(OrientToward(p, { a, c, d }, sx * cx, sy, sz * cz));
    };
    for (int i = 0; i < n; ++i)
    {
        addQuad(id(0, i), id(0, i + 1), id(1, i + 1), id(1, i), 1, 0, 1);       // outer wall, outward
        addQuad(id(2, i), id(2, i + 1), id(3, i + 1), id(3, i), -1, 0, -1);     // inner wall, toward the axis
        addQuad(id(1, i), id(1, i + 1), id(3, i + 1), id(3, i), 0, 1, 0);       // top annulus
        addQuad(id(0, i), id(0, i + 1), id(2, i + 1), id(2, i), 0, -1, 0);      // bottom annulus
    }
    return MakePiece(p, t);
}

// Case 7: a UV seam (separate corners, one vertex per column after welding) is cut without a
// break. Seam corners keep their UVs and share the vertex's position bits.
// Cut attributes run from the lower vertex to the higher one whichever way a face walks the edge:
// at y = 0.5 (t = 0.25 up the wall) every cut corner has v = 0.25.
static void TestUvSeamTubeCut()
{
    MeshGeometry tube;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildMeshGeometry(MakeTubeMeshData(8, true, 0), kDefaultEpsilonScale, tube, stats, &detail), SliceStatus::Ok, detail);
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(tube, MakePlane(0, 0.5f, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.size() == 1 && result.loops[0].size() == 16);       // 8 vertical edges + 8 diagonals
    CHECK(result.loopDepth.size() == 1 && result.loopDepth[0] == 0);
    CheckPieceValid(result.positive, "tube +");
    CheckPieceValid(result.negative, "tube -");

    int seamLow = 0, seamHigh = 0;
    for (const MeshCorner& corner : result.positive.corners)
    {
        if (result.positive.vertices[corner.vertex].cutId != 1)
            continue;
        CHECK(std::fabs(corner.uv.y - 0.25f) < 1e-5f);
        const XMFLOAT3& pos = result.positive.vertices[corner.vertex].position;
        if (std::fabs(pos.x - 1.0f) < 1e-6f && std::fabs(pos.z) < 1e-6f)
        {
            seamLow += std::fabs(corner.uv.x) < 1e-6f;
            seamHigh += std::fabs(corner.uv.x - 1.0f) < 1e-6f;
        }
    }
    CHECK(seamLow == 1 && seamHigh == 1);    // one corner per side of the seam, both on one vertex

    ProceduralMeshData baked;
    BuildRenderMesh(result.positive, baked);
    bool bitEqual = false;
    for (size_t i = 0; i < baked.vertices.size(); ++i)
        for (size_t j = i + 1; j < baked.vertices.size(); ++j)
            if (result.positive.corners[i].vertex == result.positive.corners[j].vertex
                && result.positive.corners[i].uv.x != result.positive.corners[j].uv.x)
                bitEqual = baked.vertices[i].position.x == baked.vertices[j].position.x
                    && baked.vertices[i].position.y == baked.vertices[j].position.y
                    && baked.vertices[i].position.z == baked.vertices[j].position.z;
    CHECK(bitEqual);
}

// Case 8: a seam that welding left open (the skin differs) is an UnweldedSeam boundary, and a cut
// across it is an OpenLoop.
static void TestSkinSplitSeamIsOpenLoop()
{
    MeshGeometry tube;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildMeshGeometry(MakeTubeMeshData(8, true, 1), kDefaultEpsilonScale, tube, stats, &detail), SliceStatus::Ok, detail);
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(tube, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::OpenLoop, result.detail);
    CHECK(result.detail.find("UnweldedSeam") != std::string::npos);
}

// Case 9: a thick tube cut across gives an outer loop and a hole. The hole has the opposite
// winding and sits inside the outer loop.
static void TestThickTubeHoleLoops()
{
    const MeshGeometry tube = MakeThickTube(8, 2.0f, 1.0f);
    std::string detail;
    CHECK_STATUS(ValidateMeshGeometry(tube, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(tube, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.size() == 2);
    if (result.loops.size() == 2)
    {
        CHECK(result.loops[0].size() == 16 && result.loops[1].size() == 16);
        const size_t outer = result.loopDepth[0] == 0 ? 0 : 1, hole = 1 - outer;
        CHECK(result.loopDepth[outer] == 0 && result.loopDepth[hole] == 1);
        CHECK(result.loopArea[outer] > 0.0 && result.loopArea[hole] < 0.0);
        CHECK(std::fabs(result.loopArea[outer]) > std::fabs(result.loopArea[hole]));
    }
    CheckPieceValid(result.positive, "thick tube +");
    CheckPieceValid(result.negative, "thick tube -");
}

// Two solids that touch at one vertex: that vertex has two outgoing and two incoming cut edges.
static void TestSharedNodeHasNoCap()
{
    std::vector<XMFLOAT3> p = { { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, -1, 0 } };
    std::vector<Tri> t;
    for (uint32_t i = 0; i < 4; ++i)
    {
        t.push_back(OutwardTri(p, { 4, i, (i + 1) % 4 }, { 0, 0, 0 }));
        t.push_back(OutwardTri(p, { 5, i, (i + 1) % 4 }, { 0, 0, 0 }));
    }
    // Second octahedron centered at (2,0,0): its vertex at (1,0,0) is vertex 0 of the first.
    const uint32_t b[6] = { 6, 7, 0, 8, 9, 10 };        // local 0..5 -> global
    const XMFLOAT3 local[6] = { { 3, 0, 0 }, { 2, 0, 1 }, { 1, 0, 0 }, { 2, 0, -1 }, { 2, 1, 0 }, { 2, -1, 0 } };
    p.push_back(local[0]); p.push_back(local[1]); p.push_back(local[3]); p.push_back(local[4]); p.push_back(local[5]);
    for (uint32_t i = 0; i < 4; ++i)
    {
        t.push_back(OutwardTri(p, { b[4], b[i], b[(i + 1) % 4] }, { 2, 0, 0 }));
        t.push_back(OutwardTri(p, { b[5], b[i], b[(i + 1) % 4] }, { 2, 0, 0 }));
    }
    const MeshGeometry pair = MakePiece(p, t);
    std::string detail;
    CHECK_STATUS(ValidateMeshGeometry(pair, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    GeometrySliceResult result;
    // Unreal 방식: 루프를 만들 수 없어도 절단은 되고, 모든 절단 에지가 cap 없는 경계(두 정점씩)로 남는다
    CHECK_STATUS(SliceGeometry(pair, MakePlane(0, 0, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.empty() && !result.uncapped.empty());
    for (const std::vector<uint32_t>& edge : result.uncapped)
        CHECK(edge.size() == 2);
    CHECK(CountCapFaces(result.positive) == 0 && CountKind(result.positive, MeshEdgeTag::Cut) == 0);
    CheckPieceValid(result.positive, "shared node +");
    CheckPieceValid(result.negative, "shared node -");
}

// Unreal 방식: 겹치거나, 방향이 틀리거나, 너무 깊이 중첩된 루프는 절단을 막지 않고 그 루프의 cap만 빠진다.
static void TestInvalidLoopsHaveNoCap()
{
    GeometrySliceResult result;

    MeshGeometry crossing = MakeBoxAt(0, 0, 1, 2);
    AppendPiece(crossing, MakeBoxAt(1, 1, 1, 2));
    CHECK_STATUS(SliceGeometry(crossing, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.empty() && result.uncapped.size() == 2);
    CHECK(CountCapFaces(result.positive) == 0);

    // A box inside a box with the same orientation: the inner loop is not a hole, so only the outer loop is capped.
    MeshGeometry nested = MakeBoxAt(0, 0, 3, 2);
    AppendPiece(nested, MakeBoxAt(0, 0, 1, 2));
    CHECK_STATUS(SliceGeometry(nested, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.size() == 1 && result.uncapped.size() == 1);
    CHECK(CountCapFaces(result.positive) > 0);
    CheckPieceValid(result.positive, "nested +");

    // An island inside the hole of a thick tube is nested two deep: the tube is capped with its hole, the island is not.
    MeshGeometry island = MakeThickTube(8, 3.0f, 2.0f);
    AppendPiece(island, MakeBoxAt(0, 0, 0.5f, 2));
    CHECK_STATUS(SliceGeometry(island, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.size() == 2 && result.uncapped.size() == 1);
    CheckPieceValid(result.negative, "island -");

    // A prism with a bow-tie cross section: the cut loop touches itself in the middle.
    std::vector<XMFLOAT3> p = { { -1, 0, -1 }, { 1, 0, 1 }, { 1, 0, -1 }, { -1, 0, 1 },
        { -1, 2, -1 }, { 1, 2, 1 }, { 1, 2, -1 }, { -1, 2, 1 }, { 0.3f, 0, 0.1f }, { 0.3f, 2, 0.1f } };
    std::vector<Tri> t;
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t j = (i + 1) % 4;
        t.push_back({ i, j, 4 + j });
        t.push_back({ i, 4 + j, 4 + i });
        t.push_back({ 9, 4 + i, 4 + j });
        t.push_back({ 8, j, i });
    }
    const MeshGeometry bowtie = MakePiece(p, t);
    std::string detail;
    CHECK_STATUS(ValidateMeshGeometry(bowtie, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    CHECK_STATUS(SliceGeometry(bowtie, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.empty() && result.uncapped.size() == 1);
    CHECK(!result.uncapped.empty() && result.uncapped[0].front() == result.uncapped[0].back());     // 닫힌 루프 표기

    // 근접 허용오차는 조각 크기에 비례한다: 0.0001 간격의 긴 두 상자를 아래쪽 끝 근처에서 자르면 큰 조각은 두 루프를 "닿음"으로
    // 빼고 작은 조각은 남긴다. 두 조각의 판정이 달라도 절단은 성공하고, 작은 조각에만 cap이 생긴다.
    MeshGeometry slender = MakeBoxAt(-1.00005f, 0, 1, 100);
    AppendPiece(slender, MakeBoxAt(1.00005f, 0, 1, 100));
    CHECK_STATUS(SliceGeometry(slender, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(CountCapFaces(result.positive) == 0 && CountCapFaces(result.negative) > 0);
    CheckPieceValid(result.positive, "slender +");
    CheckPieceValid(result.negative, "slender -");
}

// ---------------------------------------------------------------- 2-4 tests

static SliceOptions CapOptions()
{
    SliceOptions options;
    options.cap = CapMode::Create;
    return options;
}

static uint32_t CountOpenEdges(const MeshGeometry& piece)
{
    MeshEdgeUsageMap usage;
    CollectEdgeUsage(piece, usage);
    uint32_t n = 0;
    for (const auto& entry : usage)
        n += entry.second.count == 1;
    return n;
}

static uint32_t CountCapFaces(const MeshGeometry& piece)
{
    uint32_t n = 0;
    for (const MeshFace& face : piece.faces)
        n += face.kind == MeshFaceKind::Cap;
    return n;
}

// Dot of a cap face's geometric normal with a direction.
static double CapFacing(const MeshGeometry& piece, double dx, double dy, double dz)
{
    for (const MeshFace& face : piece.faces)
    {
        if (face.kind != MeshFaceKind::Cap)
            continue;
        const XMFLOAT3 &a = piece.vertices[piece.corners[face.corner[0]].vertex].position;
        const XMFLOAT3 &b = piece.vertices[piece.corners[face.corner[1]].vertex].position;
        const XMFLOAT3 &c = piece.vertices[piece.corners[face.corner[2]].vertex].position;
        const double e1[3] = { double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z };
        const double e2[3] = { double(c.x) - a.x, double(c.y) - a.y, double(c.z) - a.z };
        return (e1[1] * e2[2] - e1[2] * e2[1]) * dx + (e1[2] * e2[0] - e1[0] * e2[2]) * dy + (e1[0] * e2[1] - e1[1] * e2[0]) * dz;
    }
    return 0.0;
}

// Case 10: the cap is watertight. The rim has collinear vertices (the diagonal crossings sit in the
// middle of each square side), which an ear clipper that drops straight points would turn into
// T-junctions; here every cut edge is shared with exactly one cap face.
static void TestCapIsWatertight()
{
    const MeshGeometry box = MakeBox();
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CheckPieceValid(result.positive, "capped box +");
    CheckPieceValid(result.negative, "capped box -");
    CHECK(CountCapFaces(result.positive) == 6 && CountCapFaces(result.negative) == 6);   // 8-gon rim
    CHECK(CountKind(result.positive, MeshEdgeTag::Cut) == 0 && CountKind(result.negative, MeshEdgeTag::Cut) == 0);
    CHECK(CountKind(result.positive, MeshEdgeTag::CapSeam) == 8 && CountKind(result.negative, MeshEdgeTag::CapSeam) == 8);
    CHECK(CountOpenEdges(result.positive) == 0 && CountOpenEdges(result.negative) == 0);   // closed solids
    CHECK(std::fabs(TotalArea(result.positive, MeshFaceKind::Cap) - 4.0) < 1e-5);
    // The cap faces away from the kept side.
    CHECK(CapFacing(result.positive, 0, -1, 0) > 0.0 && CapFacing(result.negative, 0, 1, 0) > 0.0);
    CHECK(result.positive.caps.size() == 1 && result.positive.caps[0].rimVertices.size() == 8 && result.positive.caps[0].cutId == 1);
    CHECK(result.positive.caps[0].planeNormal.y == -1.0f && result.negative.caps[0].planeNormal.y == 1.0f);
    // Surface plus cap still covers the box once, plus two caps.
    const double original = TotalSurfaceArea(box);
    CHECK(std::fabs(TotalSurfaceArea(result.positive) + TotalSurfaceArea(result.negative) - original) < original * 1e-5);

    // Two sections: surface and cap (cap tint material slot 1).
    ProceduralMeshData baked;
    BuildRenderMesh(result.positive, baked);
    CHECK(baked.sections.size() == 2 && baked.sections[1].materialSlot == 1 && baked.sections[1].indexCount == 18);
    ProceduralMesh mesh;
    std::string error;
    CHECK(mesh.SetMesh(baked, &error));
    // Cap corners carry the planar UV and the outward normal.
    for (const MeshFace& face : result.positive.faces)
        if (face.kind == MeshFaceKind::Cap)
            for (int k = 0; k < 3; ++k)
                CHECK(result.positive.corners[face.corner[k]].normal.y == -1.0f);
}

// A tube with a hole: the cap is the outer polygon minus the hole, joined by bridge edges.
static void TestCapWithHole()
{
    const MeshGeometry tube = MakeThickTube(8, 2.0f, 1.0f);
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(tube, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.size() == 2);
    CheckPieceValid(result.positive, "capped tube +");
    CheckPieceValid(result.negative, "capped tube -");
    CHECK(CountCapFaces(result.positive) == 32 && CountCapFaces(result.negative) == 32);   // 16 + 16 + 2 holes - 2
    CHECK(CountOpenEdges(result.positive) == 0 && CountOpenEdges(result.negative) == 0);
    if (result.loopArea.size() == 2)
    {
        const double expected = std::fabs(result.loopArea[0]) - std::fabs(result.loopArea[1]);
        CHECK(std::fabs(std::fabs(TotalArea(result.positive, MeshFaceKind::Cap)) - std::fabs(expected)) < 1e-4 * std::fabs(expected));
    }
    CHECK(result.positive.caps.size() == 1 && result.positive.caps[0].loopSizes.size() == 2);
}

// A concave cross section (an L shape): ear clipping must skip the reflex corner.
static void TestCapOfConcaveLoop()
{
    const float lx[6] = { 0, 2, 2, 1, 1, 0 }, lz[6] = { 0, 0, 1, 1, 2, 2 };
    std::vector<XMFLOAT3> p;
    for (int level = 0; level < 2; ++level)
        for (int i = 0; i < 6; ++i)
            p.push_back({ lx[i], level * 2.0f, lz[i] });
    std::vector<Tri> t;
    for (uint32_t i = 1; i < 5; ++i)
    {
        t.push_back(OrientToward(p, { 6, 6 + i, 6 + i + 1 }, 0, 1, 0));      // top fan
        t.push_back(OrientToward(p, { 0, i, i + 1 }, 0, -1, 0));             // bottom fan
    }
    for (uint32_t i = 0; i < 6; ++i)
    {
        const uint32_t j = (i + 1) % 6;
        const double dx = double(lx[j]) - lx[i], dz = double(lz[j]) - lz[i];
        // The polygon runs counter-clockwise in (x, z): outward is to the right of each edge.
        t.push_back(OrientToward(p, { i, j, 6 + j }, dz, 0, -dx));
        t.push_back(OrientToward(p, { i, 6 + j, 6 + i }, dz, 0, -dx));
    }
    const MeshGeometry prism = MakePiece(p, t);
    std::string detail;
    CHECK_STATUS(ValidateMeshGeometry(prism, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(prism, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CheckPieceValid(result.positive, "L prism +");
    CheckPieceValid(result.negative, "L prism -");
    CHECK(std::fabs(TotalArea(result.positive, MeshFaceKind::Cap) - 3.0) < 1e-5);        // the L covers 3 unit squares
    CHECK(CountOpenEdges(result.positive) == 0 && CountOpenEdges(result.negative) == 0);
}

// Case 11: cutting a piece that already has a cap. A cut through the surface only is fine and the
// old cap and its seam tags stay with the piece that keeps them; a cut through the cap is rejected.
static void TestRecut()
{
    const MeshGeometry box = MakeBox();
    GeometrySliceResult first;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), first), SliceStatus::Ok, first.detail);
    const MeshGeometry upper = first.positive;       // y in [1, 2], cap at y = 1

    GeometrySliceResult second;
    CHECK_STATUS(SliceGeometry(upper, MakePlane(0, 1.5f, 0, 0, 1, 0), 2, CapOptions(), second), SliceStatus::Ok, second.detail);
    CheckPieceValid(second.positive, "recut +");
    CheckPieceValid(second.negative, "recut -");
    // The slab between the two cuts keeps the first cap; the top part has none of it.
    CHECK(second.negative.caps.size() == 2 && second.positive.caps.size() == 1);
    CHECK(second.positive.caps[0].cutId == 2 && second.negative.caps[0].cutId == 1);
    CHECK(second.loops.size() == 1);
    const size_t newSeam = second.loops.empty() ? 0 : second.loops[0].size();
    CHECK(CountKind(second.negative, MeshEdgeTag::CapSeam) == 8 + newSeam);    // inherited old seam + new seam
    CHECK(CountKind(second.positive, MeshEdgeTag::CapSeam) == newSeam);
    CHECK(CountOpenEdges(second.negative) == 0 && CountOpenEdges(second.positive) == 0);
    for (const MeshVertex& vertex : second.negative.vertices)
    {
        int used = 0;
        double sum = 0.0;
        for (int i = 0; i < 3; ++i)
            if (vertex.origin.skinVertex[i] != kInvalidIndex)
            {
                ++used;
                sum += vertex.origin.bary[i];
            }
        CHECK(used >= 1 && used <= 3 && std::fabs(sum - 1.0) < 1e-4);
    }

    // A plane that misses the piece, and one that cuts through the cap.
    GeometrySliceResult miss;
    CHECK_STATUS(SliceGeometry(upper, MakePlane(0, 0.5f, 0, 0, 1, 0), 2, CapOptions(), miss), SliceStatus::NoCut, miss.detail);
    GeometrySliceResult through;
    CHECK_STATUS(SliceGeometry(upper, MakePlane(0, 1.5f, 0, 1, 0, 0), 2, CapOptions(), through), SliceStatus::CrossesCap, through.detail);
    CHECK(through.positive.Empty() && through.negative.Empty());
    // The input piece is untouched by all of this.
    CHECK(upper.caps.size() == 1 && CountKind(upper, MeshEdgeTag::CapSeam) == 8);
}

// ---------------------------------------------------------------- debug tests (face origin)

// A surface face carries the cut that divided it; a face moved over whole keeps its old value.
// The octahedron cut through one vertex has (0, +, -) faces, whose opposite edge is divided.
static void CheckDividedFacesHaveCutId(const GeometrySliceResult& result, const MeshGeometry& input, uint32_t cutId, const char* label)
{
    // Per source triangle: faces going in, faces coming out (both pieces). More faces out than in
    // means this cut divided one of them; otherwise every face keeps a cutId it already had.
    std::vector<uint32_t> in(input.faces.size() + 64, 0), out(in.size(), 0);
    for (const MeshFace& face : input.faces)
        if (face.kind == MeshFaceKind::Surface)
            ++in[face.sourceTriangle];
    for (const MeshGeometry* piece : { &result.positive, &result.negative })
        for (const MeshFace& face : piece->faces)
            if (face.kind == MeshFaceKind::Surface)
                ++out[face.sourceTriangle];
    uint32_t divided = 0;
    for (uint32_t t = 0; t < in.size(); ++t)
    {
        if (out[t] == 0)
            continue;
        const bool wasDivided = out[t] > in[t];
        bool marked = false;
        for (const MeshGeometry* piece : { &result.positive, &result.negative })
            for (const MeshFace& face : piece->faces)
            {
                if (face.kind != MeshFaceKind::Surface || face.sourceTriangle != t)
                    continue;
                marked = marked || face.cutId == cutId;
                bool known = face.cutId == cutId && wasDivided;
                for (const MeshFace& old : input.faces)
                    known = known || (old.kind == MeshFaceKind::Surface && old.sourceTriangle == t && old.cutId == face.cutId);
                if (!known)
                {
                    std::printf("  %s: face of triangle %u has cutId %u\n", label, t, face.cutId);
                    CHECK(false);
                }
            }
        CHECK(wasDivided == marked);
        divided += wasDivided;
    }
    CHECK(divided > 0);
}

static uint32_t CountSectionFaces(const ProceduralMeshData& baked, uint32_t section)
{
    return section < baked.sections.size() ? baked.sections[section].indexCount / 3 : 0;
}

static void TestSplitFacesRecordCutId()
{
    // Plane through one vertex: (0, +, -) faces and lone-corner faces.
    const MeshGeometry octa = MakeOctahedron();
    GeometrySliceResult oneNode;
    CHECK_STATUS(SliceGeometry(octa, MakePlane(1, 0, 0, 1, 2, 0), 1, NoCapOptions(), oneNode), SliceStatus::Ok, oneNode.detail);
    CheckDividedFacesHaveCutId(oneNode, octa, 1, "octahedron");

    // Nothing was cut before, so every face is either undivided (0) or divided by cut 1.
    for (const MeshGeometry* piece : { &oneNode.positive, &oneNode.negative })
        for (const MeshFace& face : piece->faces)
            CHECK(face.cutId == 0 || face.cutId == 1);

    // Second cut: cut 2 marks the faces it divides.
    const MeshGeometry box = MakeBox();
    GeometrySliceResult first, second;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 0.5f, 0, 0.3f, 1, 0), 1, CapOptions(), first), SliceStatus::Ok, first.detail);
    CheckDividedFacesHaveCutId(first, box, 1, "box cut 1");
    CHECK_STATUS(SliceGeometry(first.positive, MakePlane(0, 1.5f, 0, 0, 1, 0.2f), 2, CapOptions(), second), SliceStatus::Ok, second.detail);
    CheckDividedFacesHaveCutId(second, first.positive, 2, "box cut 2");
    // A face moved over whole keeps the value it had (top face triangle 2 is untouched by a cut at y = 1).
    MeshGeometry marked = MakeBox();
    marked.faces[2].cutId = 7;
    GeometrySliceResult keep;
    CHECK_STATUS(SliceGeometry(marked, MakePlane(0, 1, 0, 0, 1, 0), 8, NoCapOptions(), keep), SliceStatus::Ok, keep.detail);
    uint32_t sevens = 0;
    for (const MeshFace& face : keep.positive.faces)
        sevens += face.kind == MeshFaceKind::Surface && face.sourceTriangle == 2 && face.cutId == 7;
    CHECK(sevens == 1);
    // Cap faces keep meaning "the cut that made the cap".
    for (const MeshFace& face : second.negative.faces)
        if (face.kind == MeshFaceKind::Cap)
            CHECK(face.cutId == 1 || face.cutId == 2);
}

static void TestSplitByOriginBake()
{
    const MeshGeometry box = MakeBox();
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 0.5f, 0, 0.3f, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);

    RenderBakeOptions split;
    split.splitByOrigin = true;
    for (const MeshGeometry* piece : { &result.positive, &result.negative })
    {
        ProceduralMeshData plain, tinted, defaulted;
        BuildRenderMesh(*piece, plain);
        BuildRenderMesh(*piece, defaulted, RenderBakeOptions());
        BuildRenderMesh(*piece, tinted, split);

        // The default is the old output: surface + cap, same vertices and indices as an explicit default.
        CHECK(plain.sections.size() == 2 && plain.sections[0].materialSlot == 0 && plain.sections[1].materialSlot == 1);
        CHECK(plain.indices == defaulted.indices && plain.vertices.size() == defaulted.vertices.size());

        // Split: [never divided][divided][cap]; section sizes add up to the face count.
        CHECK(tinted.sections.size() == 3);
        CHECK(tinted.sections[0].materialSlot == 0 && tinted.sections[1].materialSlot == 0 && tinted.sections[2].materialSlot == 1);
        CHECK(CountSectionFaces(tinted, 0) + CountSectionFaces(tinted, 1) + CountSectionFaces(tinted, 2) == piece->faces.size());
        CHECK(CountSectionFaces(tinted, 0) + CountSectionFaces(tinted, 1) == CountSectionFaces(plain, 0));
        CHECK(CountSectionFaces(tinted, 2) == CountSectionFaces(plain, 1));
        CHECK(tinted.sections[1].firstIndex == tinted.sections[0].indexCount);
        CHECK(tinted.vertices.size() == plain.vertices.size());

        // Each section holds the faces it names.
        uint32_t divided = 0, whole = 0;
        for (const MeshFace& face : piece->faces)
            if (face.kind == MeshFaceKind::Surface)
                (face.cutId != 0 ? divided : whole) += 1;
        CHECK(CountSectionFaces(tinted, 0) == whole && CountSectionFaces(tinted, 1) == divided);

        // The index order is the only difference: same set of triangles.
        std::vector<uint32_t> a = plain.indices, b = tinted.indices;
        std::vector<std::array<uint32_t, 3>> ta, tb;
        for (size_t i = 0; i + 2 < a.size(); i += 3)
        {
            ta.push_back({ a[i], a[i + 1], a[i + 2] });
            tb.push_back({ b[i], b[i + 1], b[i + 2] });
        }
        std::sort(ta.begin(), ta.end());
        std::sort(tb.begin(), tb.end());
        CHECK(ta == tb);

        ProceduralMesh mesh;
        std::string error;
        CHECK(mesh.SetMesh(tinted, &error));
    }

    // An empty section is not created: an undivided piece has no "divided" section.
    ProceduralMeshData untouched;
    BuildRenderMesh(box, untouched, split);
    CHECK(untouched.sections.size() == 1 && untouched.sections[0].materialSlot == 0);
}

// The preview loops: one closed ring of tubes per cut loop (6 vertices, 18 indices per edge).
static void TestLoopDebugBake()
{
    const MeshGeometry box = MakeBox();
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.size() == 1);
    ProceduralMeshData tubes;
    BakeLoopDebug(result.positive, result.loops, 0.02f, { 1.0f, 0.2f, 1.0f, 1.0f }, tubes);
    const size_t edges = result.loops.empty() ? 0 : result.loops[0].size();
    CHECK(edges == 8 && tubes.vertices.size() == edges * 6 && tubes.indices.size() == edges * 18);
    CHECK(tubes.sections.size() == 1 && tubes.sections[0].indexCount == tubes.indices.size());
    // The tubes sit on the cut plane (y = 1), around the cut loop vertices.
    for (const MeshRenderVertex& vertex : tubes.vertices)
        CHECK(std::fabs(vertex.position.y - 1.0f) <= 0.02f + 1e-5f);
    ProceduralMesh mesh;
    std::string error;
    CHECK(mesh.SetMesh(tubes, &error));

    // No loops, or loops that name missing vertices: nothing to draw.
    BakeLoopDebug(result.positive, {}, 0.02f, { 1.0f, 1.0f, 1.0f, 1.0f }, tubes);
    CHECK(tubes.vertices.empty() && tubes.sections.empty());
    BakeLoopDebug(result.positive, { { 0, 9999 } }, 0.02f, { 1.0f, 1.0f, 1.0f, 1.0f }, tubes);
    CHECK(tubes.indices.empty() && tubes.sections.empty());
}

// ---------------------------------------------------------------- cut plane tests

static double Dot3(const XMFLOAT3& a, const XMFLOAT3& b) { return double(a.x) * b.x + double(a.y) * b.y + double(a.z) * b.z; }

static XMFLOAT3 Cross3(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}

// Unit axes, mutually perpendicular, u x v = n.
// Unit axes, mutually perpendicular, u x v = n.
static bool FrameIsOrthonormal(const CutPlane::Frame& f, double tolerance)
{
    const XMFLOAT3 c = Cross3(f.axisU, f.axisV);
    return std::fabs(Dot3(f.axisU, f.axisU) - 1.0) < tolerance && std::fabs(Dot3(f.axisV, f.axisV) - 1.0) < tolerance
        && std::fabs(Dot3(f.normal, f.normal) - 1.0) < tolerance && std::fabs(Dot3(f.axisU, f.axisV)) < tolerance
        && std::fabs(Dot3(f.axisU, f.normal)) < tolerance && std::fabs(Dot3(f.axisV, f.normal)) < tolerance
        && std::fabs(c.x - f.normal.x) < tolerance && std::fabs(c.y - f.normal.y) < tolerance && std::fabs(c.z - f.normal.z) < tolerance;
}

static double QuaternionLength(const CutPlane& plane)
{
    const XMFLOAT4& q = plane.GetRotation();
    return std::sqrt(double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w);
}

// A plane at the origin with the identity rotation and the given half lengths.
static CutPlane FlatPlane(float halfX, float halfY)
{
    return CutPlane({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f }, { halfX, halfY });
}

// The plane the controller starts with: center, local +Z turned to world +Y by a -90 degree turn about X.
static CutPlane HorizontalPlane(float cx, float cy, float cz, float half)
{
    XMFLOAT4 rotation;
    XMStoreFloat4(&rotation, XMQuaternionRotationAxis(XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), -XM_PIDIV2));
    return CutPlane({ cx, cy, cz }, rotation, { half, half });
}

static void TestCutPlaneFrame()
{
    // Identity: axes are the world axes.
    const CutPlane::Frame identity = CutPlane().GetFrame();
    CHECK(identity.axisU.x == 1.0f && identity.axisV.y == 1.0f && identity.normal.z == 1.0f);
    CHECK(FrameIsOrthonormal(identity, 1e-6));

    // The starting plane is horizontal with the normal up.
    const CutPlane::Frame horizontal = HorizontalPlane(0, 0, 0, 1).GetFrame();
    CHECK(std::fabs(horizontal.normal.x) < 1e-6f && std::fabs(horizontal.normal.y - 1.0f) < 1e-6f && std::fabs(horizontal.normal.z) < 1e-6f);
    CHECK(FrameIsOrthonormal(horizontal, 1e-6));

    // Turning about each local axis keeps the frame orthonormal; a quarter turn about U maps V to the normal.
    CutPlane plane;
    for (int axis = 0; axis < 3; ++axis)
    {
        plane.RotateAroundLocalAxis(axis, 0.7f);
        CHECK(FrameIsOrthonormal(plane.GetFrame(), 1e-5));
    }
    CutPlane quarter;
    quarter.RotateAroundLocalAxis(0, XM_PIDIV2);
    const CutPlane::Frame turned = quarter.GetFrame();
    CHECK(std::fabs(turned.axisU.x - 1.0f) < 1e-6f);                     // the turn axis is unchanged
    CHECK(std::fabs(turned.normal.y + 1.0f) < 1e-5f);                    // +Z goes to -Y
    CHECK(std::fabs(turned.axisV.z - 1.0f) < 1e-5f);                     // +Y goes to +Z

    // A bad rotation counts as the identity.
    const CutPlane broken({ 0.0f, 0.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 0.0f }, { 1.0f, 1.0f });
    CHECK(FrameIsOrthonormal(broken.GetFrame(), 1e-6));
}

static void TestCutPlaneAccumulatedRotation()
{
    // Many small turns about all three local axes: the quaternion stays unit length and the frame orthonormal.
    CutPlane plane = HorizontalPlane(1, 2, 3, 0.5f);
    for (int i = 0; i < 20000; ++i)
        plane.RotateAroundLocalAxis(i % 3, (i % 2 == 0 ? 0.013f : -0.007f) * float(1 + i % 5));
    CHECK(std::fabs(QuaternionLength(plane) - 1.0) < 1e-5);
    CHECK(FrameIsOrthonormal(plane.GetFrame(), 1e-4));

    // Turns are about the plane's own axis: after tilting the plane, the next turn about U leaves U where it was.
    CutPlane tilted;
    tilted.RotateAroundLocalAxis(1, 0.9f);
    const CutPlane::Frame before = tilted.GetFrame();
    tilted.RotateAroundLocalAxis(0, 0.4f);
    const CutPlane::Frame after = tilted.GetFrame();
    CHECK(std::fabs(Dot3(before.axisU, after.axisU) - 1.0) < 1e-5);
    CHECK(std::fabs(Dot3(before.normal, after.normal) - std::cos(0.4)) < 1e-5);
}

static void TestCutPlaneContains()
{
    CutPlane plane = FlatPlane(2.0f, 1.0f);
    CHECK(plane.Contains({ 0, 0, 0 }, 0.0f));
    CHECK(plane.Contains({ 2.0f, 1.0f, 0 }, 0.0f));          // on the border
    CHECK(!plane.Contains({ 2.1f, 0, 0 }, 0.0f));
    CHECK(!plane.Contains({ 0, 1.1f, 0 }, 0.0f));
    CHECK(plane.Contains({ 2.1f, 0, 0 }, 0.2f));              // margin
    CHECK(!plane.Contains({ 2.1f, 0, 0 }, 0.05f));
    CHECK(plane.Contains({ 1.0f, 0.5f, 7.0f }, 0.0f));        // the distance along the normal is ignored

    // Turned about the normal by 90 degrees the long side points along world Y.
    plane.RotateAroundLocalAxis(2, XM_PIDIV2);
    CHECK(plane.Contains({ 0, 1.9f, 0 }, 0.0f));
    CHECK(!plane.Contains({ 1.9f, 0, 0 }, 0.0f));

    // Moved: the rectangle follows the center.
    plane.Translate({ 5.0f, 0.0f, 0.0f });
    CHECK(plane.Contains({ 5.0f, 1.9f, 0 }, 0.0f) && !plane.Contains({ 0.0f, 0.0f, 0 }, 0.0f));
}

static void TestCutPlaneScale()
{
    CutPlane plane = FlatPlane(1.0f, 0.5f);
    plane.SetExtentLimits(0.1f, 3.0f);
    plane.ScaleUniform(2.0f);
    CHECK(plane.GetHalfExtents().x == 2.0f && plane.GetHalfExtents().y == 1.0f);
    // The ratio is kept when one side reaches the limit: x would be 6, so the factor drops to 1.5.
    plane.ScaleUniform(3.0f);
    CHECK(std::fabs(plane.GetHalfExtents().x - 3.0f) < 1e-6f && std::fabs(plane.GetHalfExtents().y - 1.5f) < 1e-6f);
    plane.ScaleUniform(100.0f);
    CHECK(std::fabs(plane.GetHalfExtents().x - 3.0f) < 1e-6f && std::fabs(plane.GetHalfExtents().y - 1.5f) < 1e-6f);
    // Shrinking stops at the minimum of the smaller side.
    plane.ScaleUniform(0.0001f);
    CHECK(std::fabs(plane.GetHalfExtents().y - 0.1f) < 1e-6f && std::fabs(plane.GetHalfExtents().x - 0.2f) < 1e-6f);

    // U only: y is untouched and x stays in range.
    CutPlane blade = FlatPlane(1.0f, 1.0f);
    blade.SetExtentLimits(0.1f, 3.0f);
    blade.ScaleU(0.5f);
    CHECK(blade.GetHalfExtents().x == 0.5f && blade.GetHalfExtents().y == 1.0f);
    blade.ScaleU(1000.0f);
    CHECK(blade.GetHalfExtents().x == 3.0f && blade.GetHalfExtents().y == 1.0f);
    blade.ScaleU(1e-6f);
    CHECK(blade.GetHalfExtents().x == 0.1f);

    // Bad factors change nothing.
    blade.ScaleUniform(-1.0f);
    blade.ScaleU(0.0f);
    CHECK(blade.GetHalfExtents().x == 0.1f && blade.GetHalfExtents().y == 1.0f);

    // A plane created outside the range is clamped.
    CutPlane wild = FlatPlane(50.0f, 0.0f);
    wild.SetExtentLimits(0.1f, 3.0f);
    wild.ClampExtents();
    CHECK(wild.GetHalfExtents().x == 3.0f && wild.GetHalfExtents().y == 0.1f);
}

static void TestCutPlaneFactories()
{
    // The starting plane: horizontal at the given center, half size radius / 2, limits 0.01 and 3 times the radius.
    CutPlane start = CutPlane::MakeHorizontal({ 1.0f, 2.0f, 3.0f }, 4.0f);
    CHECK(start.GetCenter().x == 1.0f && start.GetCenter().y == 2.0f && start.GetCenter().z == 3.0f);
    CHECK(start.GetHalfExtents().x == 2.0f && start.GetHalfExtents().y == 2.0f);
    CHECK(std::fabs(start.GetFrame().normal.y - 1.0f) < 1e-6f);
    start.ScaleUniform(1000.0f);
    CHECK(std::fabs(start.GetHalfExtents().x - 12.0f) < 1e-4f);          // 3 x radius
    start.ScaleUniform(1e-9f);
    CHECK(std::fabs(start.GetHalfExtents().x - 0.04f) < 1e-6f);          // 0.01 x radius
    // A zero radius gives a zero-size plane (the limits are 0 too).
    CHECK(CutPlane::MakeHorizontal({ 0, 0, 0 }, 0.0f).GetHalfExtents().x == 0.0f);

    // MakeInfinite: the normal is the given direction, the point is the center, and the rectangle is huge.
    const XMFLOAT3 normals[] = { { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 1, 2, -3 }, { 0, 0, 5 } };
    for (const XMFLOAT3& n : normals)
    {
        const CutPlane plane = CutPlane::MakeInfinite({ 2.0f, -1.0f, 0.5f }, n);
        const double length = std::sqrt(Dot3(n, n));
        const CutPlane::Frame frame = plane.GetFrame();
        CHECK(FrameIsOrthonormal(frame, 1e-5));
        CHECK(std::fabs(frame.normal.x - n.x / length) < 1e-5 && std::fabs(frame.normal.y - n.y / length) < 1e-5
            && std::fabs(frame.normal.z - n.z / length) < 1e-5);
        CHECK(plane.GetCenter().x == 2.0f && plane.GetCenter().y == -1.0f && plane.GetCenter().z == 0.5f);
        CHECK(plane.GetHalfExtents().x == 1.0e6f && plane.GetHalfExtents().y == 1.0e6f);
        CHECK(plane.Contains({ 1.0e5f, -1.0e5f, 3.0f }, 0.0f));
    }
    // A bad normal counts as the identity rotation (the normal is +Z).
    CHECK(std::fabs(CutPlane::MakeInfinite({ 0, 0, 0 }, { 0, 0, 0 }).GetFrame().normal.z - 1.0f) < 1e-6f);
    // The infinite plane gives the same cut as the hand-made slice plane.
    const SlicePlane slice = CutPlane::MakeInfinite({ 0.0f, 1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }).ToSlicePlane();
    CHECK(std::fabs(slice.normal.y - 1.0f) < 1e-6f && std::fabs(slice.point.y - 1.0f) < 1e-6f);
}

static void TestCutPlaneToSlicePlane()
{
    // The rectangle turns into the same infinite plane as the hand-made one: the same cut comes out.
    const MeshGeometry box = MakeBox();
    GeometrySliceResult expected, actual;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), expected), SliceStatus::Ok, expected.detail);
    const SlicePlane converted = HorizontalPlane(0, 1, 0, 5.0f).ToSlicePlane();
    CHECK(std::fabs(converted.normal.y - 1.0f) < 1e-6f && std::fabs(converted.point.y - 1.0f) < 1e-6f);
    CHECK_STATUS(SliceGeometry(box, converted, 1, CapOptions(), actual), SliceStatus::Ok, actual.detail);
    CHECK(actual.positive.faces.size() == expected.positive.faces.size() && actual.negative.faces.size() == expected.negative.faces.size());
    CHECK(std::fabs(TotalSurfaceArea(actual.positive) - TotalSurfaceArea(expected.positive)) < 1e-4);

    // Turned over, the normal points down and the + and - pieces swap.
    CutPlane over = HorizontalPlane(0, 1, 0, 5.0f);
    over.RotateAroundLocalAxis(0, XM_PI);
    const SlicePlane flipped = over.ToSlicePlane();
    CHECK(flipped.normal.y < -0.99f);
    GeometrySliceResult flippedResult;
    CHECK_STATUS(SliceGeometry(box, flipped, 1, CapOptions(), flippedResult), SliceStatus::Ok, flippedResult.detail);
    CHECK(std::fabs(TotalSurfaceArea(flippedResult.positive) - TotalSurfaceArea(expected.negative)) < 1e-4);
}

static void TestLoopsInsidePlane()
{
    // The box (x, z in [-1, 1], y in [0, 2]) cut at y = 1 has a square loop with corners at +-1 and
    // collinear points in the middle of each side.
    const MeshGeometry box = MakeBox();
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.size() == 1);

    std::string detail;
    CHECK(HorizontalPlane(0, 1, 0, 1.01f).ContainsLoops(result.positive, result.loops, 0.0f, &detail));
    CHECK(!HorizontalPlane(0, 1, 0, 0.9f).ContainsLoops(result.positive, result.loops, 0.0f, &detail));
    CHECK(detail.find("outside") != std::string::npos);
    CHECK(HorizontalPlane(0, 1, 0, 0.9f).ContainsLoops(result.positive, result.loops, 0.2f, nullptr));   // margin

    // One side too short: a long thin plane misses part of the loop, a plane off to the side too.
    const CutPlane blade = CutPlane({ 0.0f, 1.0f, 0.0f }, HorizontalPlane(0, 1, 0, 1.01f).GetRotation(), { 0.5f, 1.01f });
    CHECK(!blade.ContainsLoops(result.positive, result.loops, 0.0f, nullptr));
    const CutPlane shifted = HorizontalPlane(0.6f, 1, 0, 1.01f);
    CHECK(!shifted.ContainsLoops(result.positive, result.loops, 0.0f, nullptr));

    // Turned about the normal by 45 degrees the square loop's corners reach 1.41 along the plane axes.
    CutPlane turned = HorizontalPlane(0, 1, 0, 1.2f);
    turned.RotateAroundLocalAxis(2, XM_PIDIV4);
    CHECK(!turned.ContainsLoops(result.positive, result.loops, 0.0f, nullptr));
    const CutPlane bigger(turned.GetCenter(), turned.GetRotation(), { 1.5f, 1.5f });
    CHECK(bigger.ContainsLoops(result.positive, result.loops, 0.0f, nullptr));

    // No loops (nothing to check) passes; a loop that names a missing vertex fails.
    const std::vector<std::vector<uint32_t>> none;
    CHECK(CutPlane().ContainsLoops(result.positive, none, 0.0f, nullptr));
    const std::vector<std::vector<uint32_t>> missing = { { 0, 9999 } };
    CHECK(!HorizontalPlane(0, 1, 0, 5.0f).ContainsLoops(result.positive, missing, 0.0f, nullptr));
}

// ---------------------------------------------------------------- segment pick and joint set tests

// Joints stacked along Y: joint i at (0, i, 0), parent i - 1.
static SkeletonPose MakeChainPose(uint32_t count)
{
    SkeletonPose pose;
    for (uint32_t i = 0; i < count; ++i)
    {
        pose.jointPositions.push_back({ 0.0f, float(i), 0.0f });
        pose.parents.push_back(i == 0 ? kInvalidIndex : i - 1);
    }
    return pose;
}

// Root (0,2,0) with two legs: hip 1 and knee 2 on the left (x = -0.5), hip 3 and knee 4 on the right (x = +0.5).
static SkeletonPose MakeLegsPose()
{
    SkeletonPose pose;
    pose.jointPositions = { { 0.0f, 2.0f, 0.0f }, { -0.5f, 1.8f, 0.0f }, { -0.5f, 1.0f, 0.0f }, { 0.5f, 1.8f, 0.0f }, { 0.5f, 1.0f, 0.0f } };
    pose.parents = { kInvalidIndex, 0, 1, 0, 3 };
    return pose;
}

static void TestPickChain()
{
    const SkeletonPose chain = MakeChainPose(5);       // joints at y = 0..4
    std::vector<SegmentHit> hits;

    // Between joints 1 and 2: the parent end (joint 1) is the cut joint.
    HorizontalPlane(0, 1.5f, 0, 1.0f).PickSegments(chain, hits);
    CHECK(hits.size() == 1 && hits[0].parentJoint == 1 && hits[0].childJoint == 2);
    CHECK(!hits.empty() && std::fabs(hits[0].t - 0.5f) < 1e-6f && std::fabs(hits[0].point.y - 1.5f) < 1e-6f);
    CHECK(CutPlane::CutJoints(hits) == std::vector<uint32_t>({ 1 }));

    // A quarter of the way down the bone: t is measured from the parent.
    HorizontalPlane(0, 2.25f, 0, 1.0f).PickSegments(chain, hits);
    CHECK(hits.size() == 1 && hits[0].parentJoint == 2 && std::fabs(hits[0].t - 0.25f) < 1e-6f);

    // The flipped plane (normal down) crosses the same segment.
    CutPlane flipped = HorizontalPlane(0, 1.5f, 0, 1.0f);
    flipped.RotateAroundLocalAxis(0, XM_PI);
    flipped.PickSegments(chain, hits);
    CHECK(hits.size() == 1 && hits[0].parentJoint == 1 && hits[0].childJoint == 2);

    // Beyond both ends of the chain: nothing to cut (no virtual segment past the last joint).
    HorizontalPlane(0, 10.0f, 0, 1.0f).PickSegments(chain, hits);
    CHECK(hits.empty());
    HorizontalPlane(0, -5.0f, 0, 1.0f).PickSegments(chain, hits);
    CHECK(hits.empty());
}

static void TestPickThroughJoint()
{
    // A plane exactly through a joint belongs to one segment only: the joint counts as the + side.
    const SkeletonPose chain = MakeChainPose(5);
    std::vector<SegmentHit> hits;
    HorizontalPlane(0, 2.0f, 0, 1.0f).PickSegments(chain, hits);
    CHECK(hits.size() == 1 && hits[0].parentJoint == 1 && hits[0].childJoint == 2 && hits[0].t == 1.0f);

    // Through the last joint: the segment above it, at its end.
    HorizontalPlane(0, 4.0f, 0, 1.0f).PickSegments(chain, hits);
    CHECK(hits.size() == 1 && hits[0].parentJoint == 3 && hits[0].childJoint == 4 && hits[0].t == 1.0f);

    // Through the root: the root is on the + side with everything above it, so no segment is crossed.
    HorizontalPlane(0, 0.0f, 0, 1.0f).PickSegments(chain, hits);
    CHECK(hits.empty());

    // With the normal pointing down the same plane through joint 2 goes to the next segment instead,
    // but it is still exactly one segment.
    CutPlane flipped = HorizontalPlane(0, 2.0f, 0, 1.0f);
    flipped.RotateAroundLocalAxis(0, XM_PI);
    flipped.PickSegments(chain, hits);
    CHECK(hits.size() == 1 && hits[0].parentJoint == 2 && hits[0].childJoint == 3 && hits[0].t == 0.0f);
}

static void TestPickSeveralSegments()
{
    const SkeletonPose legs = MakeLegsPose();
    std::vector<SegmentHit> hits;

    // A wide plane across both thighs: two hits in child order, two cut joints.
    HorizontalPlane(0, 1.4f, 0, 1.0f).PickSegments(legs, hits);
    CHECK(hits.size() == 2 && hits[0].childJoint == 2 && hits[1].childJoint == 4);
    CHECK(hits.size() == 2 && hits[0].parentJoint == 1 && hits[1].parentJoint == 3);
    CHECK(CutPlane::CutJoints(hits) == std::vector<uint32_t>({ 1, 3 }));
    CHECK(hits.size() == 2 && hits[0].point.x == -0.5f && hits[1].point.x == 0.5f);

    // Too small to reach the right leg: only the left one is crossed.
    CutPlane left = HorizontalPlane(-0.5f, 1.4f, 0, 0.3f);
    left.PickSegments(legs, hits);
    CHECK(hits.size() == 1 && hits[0].childJoint == 2);

    // A thin plane (U only) turned to run along X catches both; turned to run along Z it misses both.
    CutPlane blade = CutPlane({ 0.0f, 1.4f, 0.0f }, HorizontalPlane(0, 1.4f, 0, 1.0f).GetRotation(), { 1.0f, 0.1f });
    blade.PickSegments(legs, hits);
    CHECK(hits.size() == 2);
    blade.RotateAroundLocalAxis(2, XM_PIDIV2);
    blade.PickSegments(legs, hits);
    CHECK(hits.empty());

    // A plane that crosses nothing, and one far away.
    HorizontalPlane(0, 0.2f, 0, 1.0f).PickSegments(legs, hits);
    CHECK(hits.empty());
    HorizontalPlane(5.0f, 1.4f, 0, 1.0f).PickSegments(legs, hits);
    CHECK(hits.empty());
}

static void TestPickSharedParent()
{
    // Joint 1 has two children (2 and 3): a plane across both bones gives two hits and one cut joint.
    SkeletonPose pose;
    pose.jointPositions = { { 0.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { -1.0f, 2.0f, 0.0f }, { 1.0f, 2.0f, 0.0f } };
    pose.parents = { kInvalidIndex, 0, 1, 1 };
    std::vector<SegmentHit> hits;
    HorizontalPlane(0, 1.5f, 0, 2.0f).PickSegments(pose, hits);
    CHECK(hits.size() == 2 && hits[0].parentJoint == 1 && hits[1].parentJoint == 1);
    CHECK(CutPlane::CutJoints(hits) == std::vector<uint32_t>({ 1 }));

    // Only one of the two bones is crossed when the plane is small: still joint 1.
    HorizontalPlane(-0.5f, 1.5f, 0, 0.2f).PickSegments(pose, hits);
    CHECK(hits.size() == 1 && hits[0].childJoint == 2);
    CHECK(CutPlane::CutJoints(hits) == std::vector<uint32_t>({ 1 }));
}

static void TestPickRejectsBadInput()
{
    std::vector<SegmentHit> hits;
    SkeletonPose bad = MakeChainPose(3);
    bad.parents.pop_back();
    hits.push_back(SegmentHit());
    HorizontalPlane(0, 0.5f, 0, 1.0f).PickSegments(bad, hits);
    CHECK(hits.empty());                       // sizes differ; hits is cleared first

    SkeletonPose selfParent = MakeChainPose(3);
    selfParent.parents[1] = 1;
    selfParent.parents[2] = 99;
    HorizontalPlane(0, 1.5f, 0, 1.0f).PickSegments(selfParent, hits);
    CHECK(hits.empty());                       // a joint that is its own parent or has a missing parent is skipped

    CHECK(CutPlane::CutJoints({}).empty());
    SegmentHit a, b;
    a.parentJoint = 4;
    b.parentJoint = 2;
    CHECK(CutPlane::CutJoints({ a, b, a }) == std::vector<uint32_t>({ 2, 4 }));    // sorted, no duplicates
}

static void MakeVertex(uint32_t seed, uint16_t joints[4], float weights[4])
{
    // Deterministic pseudo-random skin: up to four slots, small joint numbers so slots often repeat.
    uint32_t state = seed * 2654435761u + 12345u;
    const auto next = [&state]() { state = state * 1664525u + 1013904223u; return (state >> 8) & 0xffff; };
    float sum = 0.0f;
    for (int i = 0; i < 4; ++i)
    {
        joints[i] = uint16_t(next() % 6);
        weights[i] = (next() % 3 == 0) ? 0.0f : float(next() % 1000) / 1000.0f;
        sum += weights[i];
    }
    for (int i = 0; i < 4; ++i)
        weights[i] = sum > 0.0f ? weights[i] / sum : 0.0f;
}

static void TestJointSet()
{
    // Slots holding the same joint add up.
    const uint16_t joints[4] = { 3, 5, 3, 0 };
    const float weights[4] = { 0.25f, 0.5f, 0.125f, 0.125f };
    CHECK(Skinning::Influence(joints, weights, 3) == 0.375f && Skinning::Influence(joints, weights, 5) == 0.5f);
    CHECK(Skinning::Influence(joints, weights, 7) == 0.0f);
    CHECK(Skinning::SetInfluence(joints, weights, { 3, 5 }) == 0.875f && Skinning::SetInfluence(joints, weights, {}) == 0.0f);

    // A set with one joint is that joint, for every minWeight.
    const float minWeights[3] = { 0.0f, 0.1f, 0.5f };
    for (uint32_t v = 0; v < 400; ++v)
    {
        uint16_t j[4];
        float w[4];
        MakeVertex(v, j, w);
        for (uint32_t joint = 0; joint < 6; ++joint)
            for (float minWeight : minWeights)
                CHECK(Skinning::InJointSet(j, w, { joint }, minWeight) == (Skinning::Influence(j, w, joint) > minWeight));
    }

    // A set of two is the union of the two single joint results, for vertices and for whole triangles
    // (a triangle is taken when any of its vertices is).
    for (uint32_t v = 0; v < 300; ++v)
    {
        uint16_t j[3][4];
        float w[3][4];
        for (int k = 0; k < 3; ++k)
            MakeVertex(v * 3 + k, j[k], w[k]);
        for (float minWeight : minWeights)
        {
            bool inA = false, inB = false, inBoth = false;
            for (int k = 0; k < 3; ++k)
            {
                inA = inA || Skinning::InJointSet(j[k], w[k], { 1 }, minWeight);
                inB = inB || Skinning::InJointSet(j[k], w[k], { 4 }, minWeight);
                inBoth = inBoth || Skinning::InJointSet(j[k], w[k], { 1, 4 }, minWeight);
            }
            CHECK(inBoth == (inA || inB));
        }
    }

    // A joint nobody uses selects nothing; an empty set selects nothing.
    uint16_t j[4];
    float w[4];
    MakeVertex(7, j, w);
    CHECK(!Skinning::InJointSet(j, w, { 9 }, 0.0f) && !Skinning::InJointSet(j, w, {}, 0.0f));

    // Valid sets: ascending, no duplicates, inside the palette, not empty.
    CHECK(Skinning::IsValidJointSet({ 0 }, 1) && Skinning::IsValidJointSet({ 1, 3, 4 }, 5));
    CHECK(!Skinning::IsValidJointSet({}, 5) && !Skinning::IsValidJointSet({ 3, 1 }, 5) && !Skinning::IsValidJointSet({ 1, 1 }, 5) && !Skinning::IsValidJointSet({ 5 }, 5));
}

// ---------------------------------------------------------------- plane gizmo tests (Geometry/CutPlaneGizmo.cpp)

// eye (0, 0, -5) looking at the origin, 60 degree vertical fov, near 0.1, far 100. The reversed variant swaps
// near and far like MiniEngine does (depth 1 at the near plane, 0 at the far plane).
static GizmoView MakeTestView(bool reversedZ, float width = 1600.0f, float height = 900.0f, float eyeZ = -5.0f)
{
    const XMFLOAT3 eye = { 0.0f, 0.0f, eyeZ };
    const float fovY = XM_PI / 3.0f;
    const XMMATRIX view = XMMatrixLookAtLH(XMLoadFloat3(&eye), XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const float aspect = width / height;
    const XMMATRIX projection = reversedZ ? XMMatrixPerspectiveFovLH(fovY, aspect, 100.0f, 0.1f)
        : XMMatrixPerspectiveFovLH(fovY, aspect, 0.1f, 100.0f);
    XMFLOAT4X4 viewProj;
    XMStoreFloat4x4(&viewProj, view * projection);
    return GizmoView::Make(viewProj, eye, width, height, reversedZ);
}

static GizmoRay MakeTestRay(float ox, float oy, float oz, float dx, float dy, float dz)
{
    GizmoRay ray;
    ray.origin = { ox, oy, oz };
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    ray.direction = { dx / length, dy / length, dz / length };
    return ray;
}

static GizmoPointer MakeTestPointer(float x, float y, const GizmoRay& ray)
{
    GizmoPointer pointer;
    pointer.x = x;
    pointer.y = y;
    pointer.ray = ray;
    return pointer;
}

// One drag update on `target` with the size limits [lo, hi]. The plane is changed only when it returns true.
static bool DragTo(CutPlane& target, GizmoDrag& drag, const GizmoPointer& pointer, float lo, float hi)
{
    target.SetExtentLimits(lo, hi);
    return target.UpdateDrag(drag, pointer);
}

static void TestGizmoRay()
{
    for (int variant = 0; variant < 2; ++variant)
    {
        const bool reversed = variant == 1;
        const GizmoView view = MakeTestView(reversed);

        // The middle of the window looks straight ahead (+Z), from a point on the near plane.
        const GizmoRay center = CutPlane::MakeRay(view, 800.0f, 450.0f);
        CHECK(std::fabs(center.direction.x) < 1e-4f && std::fabs(center.direction.y) < 1e-4f && std::fabs(center.direction.z - 1.0f) < 1e-4f);
        CHECK(std::fabs(center.origin.z - (-4.9f)) < 1e-3f);

        // The corners match the field of view: tan(30 degrees) vertically, times the aspect ratio horizontally.
        const float tanHalf = std::tan(XM_PI / 6.0f), aspect = 1600.0f / 900.0f;
        const GizmoRay topLeft = CutPlane::MakeRay(view, 0.0f, 0.0f);
        CHECK(std::fabs(topLeft.direction.x / topLeft.direction.z + tanHalf * aspect) < 1e-3f);
        CHECK(std::fabs(topLeft.direction.y / topLeft.direction.z - tanHalf) < 1e-3f);
        const GizmoRay bottomRight = CutPlane::MakeRay(view, 1600.0f, 900.0f);
        CHECK(std::fabs(bottomRight.direction.x / bottomRight.direction.z - tanHalf * aspect) < 1e-3f);
        CHECK(std::fabs(bottomRight.direction.y / bottomRight.direction.z + tanHalf) < 1e-3f);

        // Every ray passes through the eye.
        const XMVECTOR toEye = XMVectorSet(view.eye.x - topLeft.origin.x, view.eye.y - topLeft.origin.y, view.eye.z - topLeft.origin.z, 0.0f);
        const XMVECTOR dir = XMVectorSet(topLeft.direction.x, topLeft.direction.y, topLeft.direction.z, 0.0f);
        CHECK(XMVectorGetX(XMVector3Length(XMVector3Cross(dir, toEye))) < 1e-3f);

        // A point on a ray projects back to the pixel the ray came from.
        const GizmoRay ray = CutPlane::MakeRay(view, 400.0f, 300.0f);
        const XMFLOAT3 onRay = { ray.origin.x + ray.direction.x * 3.0f, ray.origin.y + ray.direction.y * 3.0f, ray.origin.z + ray.direction.z * 3.0f };
        float px = 0.0f, py = 0.0f;
        CHECK(CutPlane::ProjectToScreen(onRay, view, px, py) && std::fabs(px - 400.0f) < 0.5f && std::fabs(py - 300.0f) < 0.5f);

        // Behind the camera there is no screen position.
        CHECK(!CutPlane::ProjectToScreen({ 0.0f, 0.0f, -9.0f }, view, px, py));
    }
}

static void TestGizmoAxisDrag()
{
    float param = 0.0f;
    // A ray along -Y through (2, 3, 4) passes the X axis at x = 2.
    CHECK(CutPlane::ClosestParamOnAxis({ 0, 0, 0 }, { 1, 0, 0 }, MakeTestRay(2, 3, 4, 0, -1, 0), param) && std::fabs(param - 2.0f) < 1e-5f);
    // The axis does not have to start at the origin, nor point along +X.
    CHECK(CutPlane::ClosestParamOnAxis({ 5, 0, 0 }, { 0, 0, 1 }, MakeTestRay(4, 3, 7, 0, -1, 0), param) && std::fabs(param - 7.0f) < 1e-5f);
    CHECK(CutPlane::ClosestParamOnAxis({ 0, 0, 0 }, { 0, 0, -1 }, MakeTestRay(0, 1, 6, 1, 0, 0), param) && std::fabs(param + 6.0f) < 1e-5f);
    // Parallel and almost parallel rays give no answer instead of a huge number.
    CHECK(!CutPlane::ClosestParamOnAxis({ 0, 0, 0 }, { 1, 0, 0 }, MakeTestRay(0, 1, 0, 1, 0, 0), param));
    CHECK(!CutPlane::ClosestParamOnAxis({ 0, 0, 0 }, { 1, 0, 0 }, MakeTestRay(0, 1, 0, 1, 1e-4f, 0), param));
    CHECK(CutPlane::ClosestParamOnAxis({ 0, 0, 0 }, { 1, 0, 0 }, MakeTestRay(0, 1, 0, 1, 0.05f, 0.05f), param) && std::isfinite(param));

    // Ray and plane.
    XMFLOAT3 hit;
    CHECK(CutPlane::RayPlaneHit(MakeTestRay(1, 2, -5, 0, 0, 1), { 0, 0, 0 }, { 0, 0, 1 }, hit) && std::fabs(hit.x - 1.0f) < 1e-5f && std::fabs(hit.y - 2.0f) < 1e-5f && std::fabs(hit.z) < 1e-5f);
    CHECK(!CutPlane::RayPlaneHit(MakeTestRay(1, 2, -5, 1, 0, 0), { 0, 0, 0 }, { 0, 0, 1 }, hit));     // parallel
    CHECK(!CutPlane::RayPlaneHit(MakeTestRay(1, 2, -5, 0, 0, -1), { 0, 0, 0 }, { 0, 0, 1 }, hit));    // the plane is behind the ray

    // Dragging the X arrow: the center follows the closest point on the axis and nothing else moves.
    CutPlane plane;
    GizmoDrag drag;
    CHECK(plane.BeginDrag(GizmoHandle::MoveX, MakeTestPointer(0, 0, MakeTestRay(1, 0, -5, 0, 0, 1)), drag));
    CutPlane moved;
    CHECK(DragTo(moved, drag, MakeTestPointer(0, 0, MakeTestRay(3, 1.5f, -5, 0, 0, 1)), 0.1f, 3.0f));
    CHECK(std::fabs(moved.GetCenter().x - 2.0f) < 1e-5f && moved.GetCenter().y == 0.0f && moved.GetCenter().z == 0.0f);
    CHECK(DragTo(moved, drag, MakeTestPointer(0, 0, MakeTestRay(-2, 0, -5, 0, 0, 1)), 0.1f, 3.0f));
    CHECK(std::fabs(moved.GetCenter().x + 3.0f) < 1e-5f);
    // The result always starts from the plane the drag began with, so nothing adds up between updates.
    CHECK(DragTo(moved, drag, MakeTestPointer(0, 0, MakeTestRay(1, 0, -5, 0, 0, 1)), 0.1f, 3.0f) && std::fabs(moved.GetCenter().x) < 1e-5f);
    // A ray parallel to the axis cannot be used: the plane is left as it was.
    CutPlane untouched({ 42.0f, 0.0f, 0.0f }, moved.GetRotation(), moved.GetHalfExtents());
    CHECK(!DragTo(untouched, drag, MakeTestPointer(0, 0, MakeTestRay(0, 1, 0, 1, 0, 0)), 0.1f, 3.0f) && untouched.GetCenter().x == 42.0f);

    // Dragging the XY square moves both axes by what the hit point on the plane moved.
    CHECK(plane.BeginDrag(GizmoHandle::MoveXY, MakeTestPointer(0, 0, MakeTestRay(1, 1, -5, 0, 0, 1)), drag));
    CHECK(DragTo(moved, drag, MakeTestPointer(0, 0, MakeTestRay(4, -2, -5, 0, 0, 1)), 0.1f, 3.0f));
    CHECK(std::fabs(moved.GetCenter().x - 3.0f) < 1e-5f && std::fabs(moved.GetCenter().y + 3.0f) < 1e-5f && std::fabs(moved.GetCenter().z) < 1e-5f);
    // A ray lying in the plane cannot start the drag.
    CHECK(!plane.BeginDrag(GizmoHandle::MoveXY, MakeTestPointer(0, 0, MakeTestRay(-5, 1, 0, 1, 0, 0)), drag));
}

static void TestGizmoRotateDrag()
{
    CutPlane plane;     // center at the origin, local axes = world axes
    GizmoDrag drag;
    CutPlane turned;
    const auto ray = [](float x, float y) { return MakeTestRay(x, y, -5, 0, 0, 1); };

    // The normal ring: from (1, 0) to (0, 1) is +90 degrees about +Z, which turns U onto +Y.
    CHECK(plane.BeginDrag(GizmoHandle::RotateN, MakeTestPointer(0, 0, ray(1, 0)), drag));
    CHECK(DragTo(turned, drag, MakeTestPointer(0, 0, ray(0, 2)), 0.1f, 3.0f));
    CutPlane::Frame frame = turned.GetFrame();
    CHECK(std::fabs(frame.axisU.x) < 1e-5f && std::fabs(frame.axisU.y - 1.0f) < 1e-5f && FrameIsOrthonormal(frame, 1e-5));
    CHECK(std::fabs(QuaternionLength(turned) - 1.0) < 1e-6);

    // The other way round is -90 degrees.
    CHECK(plane.BeginDrag(GizmoHandle::RotateN, MakeTestPointer(0, 0, ray(1, 0)), drag));
    CHECK(DragTo(turned, drag, MakeTestPointer(0, 0, ray(0, -1)), 0.1f, 3.0f));
    frame = turned.GetFrame();
    CHECK(std::fabs(frame.axisU.y + 1.0f) < 1e-5f);

    // Going around more than half a turn keeps adding up: 100, 200 and 300 degrees.
    CHECK(plane.BeginDrag(GizmoHandle::RotateN, MakeTestPointer(0, 0, ray(1, 0)), drag));
    const float degrees[3] = { 100.0f, 200.0f, 300.0f };
    for (float d : degrees)
    {
        const float radians = d * XM_PI / 180.0f;
        CHECK(DragTo(turned, drag, MakeTestPointer(0, 0, ray(std::cos(radians), std::sin(radians))), 0.1f, 3.0f));
        frame = turned.GetFrame();
        CHECK(std::fabs(frame.axisU.x - std::cos(radians)) < 1e-4f && std::fabs(frame.axisU.y - std::sin(radians)) < 1e-4f);
    }
    CHECK(std::fabs(drag.totalAngle - 300.0f * XM_PI / 180.0f) < 1e-4f);

    // The U ring turns about the plane's own U axis (here +X): Y goes to Z, so the normal ends up at -Y.
    CHECK(plane.BeginDrag(GizmoHandle::RotateU, MakeTestPointer(0, 0, MakeTestRay(-5, 1, 0, 1, 0, 0)), drag));
    CHECK(DragTo(turned, drag, MakeTestPointer(0, 0, MakeTestRay(-5, 0, 1, 1, 0, 0)), 0.1f, 3.0f));
    frame = turned.GetFrame();
    CHECK(std::fabs(frame.normal.y + 1.0f) < 1e-5f && std::fabs(frame.axisU.x - 1.0f) < 1e-5f);

    // The turn follows the plane's own axis once the plane is already rotated.
    CutPlane tilted;
    tilted.RotateAroundLocalAxis(1, 0.7f);
    const CutPlane::Frame tiltedFrame = tilted.GetFrame();
    const XMFLOAT3 center = tilted.GetCenter();
    // A ray along the U axis hits the ring's plane at the center; use one slightly off it, perpendicular to U.
    const GizmoRay a = MakeTestRay(center.x - tiltedFrame.axisU.x * 5.0f + tiltedFrame.axisV.x, center.y - tiltedFrame.axisU.y * 5.0f + tiltedFrame.axisV.y,
        center.z - tiltedFrame.axisU.z * 5.0f + tiltedFrame.axisV.z, tiltedFrame.axisU.x, tiltedFrame.axisU.y, tiltedFrame.axisU.z);
    const GizmoRay b = MakeTestRay(center.x - tiltedFrame.axisU.x * 5.0f + tiltedFrame.normal.x, center.y - tiltedFrame.axisU.y * 5.0f + tiltedFrame.normal.y,
        center.z - tiltedFrame.axisU.z * 5.0f + tiltedFrame.normal.z, tiltedFrame.axisU.x, tiltedFrame.axisU.y, tiltedFrame.axisU.z);
    CHECK(tilted.BeginDrag(GizmoHandle::RotateU, MakeTestPointer(0, 0, a), drag));
    CHECK(DragTo(turned, drag, MakeTestPointer(0, 0, b), 0.1f, 3.0f));
    const CutPlane::Frame after = turned.GetFrame();
    CHECK(std::fabs(Dot3(after.axisU, tiltedFrame.axisU) - 1.0) < 1e-4);         // the axis it turned about stays
    CHECK(std::fabs(Dot3(after.axisV, tiltedFrame.normal) - 1.0) < 1e-4);        // V went to the old normal (+90 degrees)
}

static void TestGizmoScaleDrag()
{
    CutPlane plane = FlatPlane(2.0f, 1.0f);
    GizmoDrag drag;
    CutPlane scaled;
    const auto ray = [](float x, float y) { return MakeTestRay(x, y, -5, 0, 0, 1); };

    // The U handle sits at the edge; dragging it by 1.5 widens the plane by 1.5. Only the U half length changes.
    CHECK(plane.BeginDrag(GizmoHandle::ScaleU, MakeTestPointer(0, 0, ray(2, 0)), drag));
    CHECK(DragTo(scaled, drag, MakeTestPointer(0, 0, ray(3.5f, 0.7f)), 0.1f, 4.0f));
    CHECK(std::fabs(scaled.GetHalfExtents().x - 3.5f) < 1e-5f && scaled.GetHalfExtents().y == 1.0f);
    // The range is kept at both ends.
    CHECK(DragTo(scaled, drag, MakeTestPointer(0, 0, ray(100, 0)), 0.1f, 3.0f) && scaled.GetHalfExtents().x == 3.0f);
    CHECK(DragTo(scaled, drag, MakeTestPointer(0, 0, ray(-100, 0)), 0.1f, 3.0f) && scaled.GetHalfExtents().x == 0.1f);

    // The V handle works on the other half length.
    CHECK(plane.BeginDrag(GizmoHandle::ScaleV, MakeTestPointer(0, 0, MakeTestRay(-5, 1, 0, 1, 0, 0)), drag));
    CHECK(DragTo(scaled, drag, MakeTestPointer(0, 0, MakeTestRay(-5, 2.5f, 0, 1, 0, 0)), 0.1f, 4.0f));
    CHECK(scaled.GetHalfExtents().x == 2.0f && std::fabs(scaled.GetHalfExtents().y - 2.5f) < 1e-5f);

    // The center handle scales both sides with the cursor: up and right grows, down and left shrinks, e per 150 pixels.
    CHECK(plane.BeginDrag(GizmoHandle::ScaleUniform, MakeTestPointer(800, 450, ray(0, 0)), drag));
    CHECK(DragTo(scaled, drag, MakeTestPointer(800, 300, ray(0, 0)), 0.1f, 10.0f));
    CHECK(std::fabs(scaled.GetHalfExtents().x - 2.0f * std::exp(1.0f)) < 1e-4f && std::fabs(scaled.GetHalfExtents().y - std::exp(1.0f)) < 1e-4f);
    CHECK(DragTo(scaled, drag, MakeTestPointer(650, 450, ray(0, 0)), 0.1f, 10.0f));
    CHECK(std::fabs(scaled.GetHalfExtents().x - 2.0f * std::exp(-1.0f)) < 1e-4f);
    // At the limit the ratio of the two sides is kept: 2 x 1 can grow to 3 x 1.5 at most.
    CHECK(DragTo(scaled, drag, MakeTestPointer(1500, 0, ray(0, 0)), 0.1f, 3.0f));
    CHECK(std::fabs(scaled.GetHalfExtents().x - 3.0f) < 1e-5f && std::fabs(scaled.GetHalfExtents().y - 1.5f) < 1e-5f);

    // There is no handle for the normal.
    CHECK(!plane.BeginDrag(GizmoHandle::None, MakeTestPointer(0, 0, ray(0, 0)), drag));
}

static void TestGizmoPicking()
{
    const GizmoView view = MakeTestView(true);
    CutPlane plane;     // at the origin, looking at it from (0, 0, -5)

    // The gizmo is about 100 pixels big wherever the camera is.
    for (float eyeZ : { -5.0f, -12.0f })
    {
        const GizmoView v = MakeTestView(true, 1600.0f, 900.0f, eyeZ);
        const float unit = CutPlane::GetGizmoUnit(v, plane.GetCenter());
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;
        CHECK(CutPlane::ProjectToScreen({ 0, 0, 0 }, v, x0, y0) && CutPlane::ProjectToScreen({ unit, 0, 0 }, v, x1, y1));
        CHECK(std::fabs(std::hypot(x1 - x0, y1 - y0) - 100.0f) < 3.0f);
    }

    // The X arrow: its middle is on the handle, a few pixels off is still on it, far away is not.
    const std::vector<GizmoPrimitive> move = plane.BuildGizmoPrimitives(view, GizmoTool::Move);
    CHECK(move.size() == 6);
    const GizmoPrimitive* xArrow = nullptr;
    const GizmoPrimitive* xySquare = nullptr;
    for (const GizmoPrimitive& p : move)
    {
        if (p.handle == GizmoHandle::MoveX)
            xArrow = &p;
        if (p.handle == GizmoHandle::MoveXY)
            xySquare = &p;
    }
    CHECK(xArrow != nullptr && xySquare != nullptr);
    if (xArrow != nullptr && xySquare != nullptr)
    {
        float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
        CHECK(CutPlane::ProjectToScreen(xArrow->points[0], view, ax, ay) && CutPlane::ProjectToScreen(xArrow->points[1], view, bx, by));
        const float mx = (ax + bx) * 0.5f, my = (ay + by) * 0.5f;
        CHECK(plane.PickHandle(view, GizmoTool::Move, mx, my) == GizmoHandle::MoveX);
        CHECK(plane.PickHandle(view, GizmoTool::Move, mx, my + 7.0f) == GizmoHandle::MoveX);
        CHECK(plane.PickHandle(view, GizmoTool::Move, mx, my + 9.0f) == GizmoHandle::None);
        CHECK(plane.PickHandle(view, GizmoTool::Move, mx, my + 30.0f) == GizmoHandle::None);
        // The middle of the XY square is on the square (a filled cell, not just its border).
        float cx = 0.0f, cy = 0.0f;
        float sx = 0.0f, sy = 0.0f;
        CHECK(CutPlane::ProjectToScreen(xySquare->points[0], view, cx, cy) && CutPlane::ProjectToScreen(xySquare->points[2], view, sx, sy));
        CHECK(plane.PickHandle(view, GizmoTool::Move, (cx + sx) * 0.5f, (cy + sy) * 0.5f) == GizmoHandle::MoveXY);
    }

    // The rings are picked on the ring line, not inside.
    const std::vector<GizmoPrimitive> rings = plane.BuildGizmoPrimitives(view, GizmoTool::Rotate);
    CHECK(rings.size() == 3);
    float rx = 0.0f, ry = 0.0f;
    CHECK(!rings.empty() && CutPlane::ProjectToScreen(rings[2].points[0], view, rx, ry));
    CHECK(rings.size() == 3 && plane.PickHandle(view, GizmoTool::Rotate, rx, ry) != GizmoHandle::None);
    // The U and V rings are seen edge-on from here and cross the center as lines, so test a point inside the N ring
    // but away from those lines.
    CHECK(plane.PickHandle(view, GizmoTool::Rotate, 840.0f, 410.0f) == GizmoHandle::None);

    // The scale tool: the center handle, and the two edge handles at the half lengths.
    const CutPlane wide = FlatPlane(2.0f, 1.0f);
    float ux = 0.0f, uy = 0.0f, cx = 0.0f, cy = 0.0f;
    CHECK(CutPlane::ProjectToScreen({ 2.0f, 0.0f, 0.0f }, view, ux, uy) && CutPlane::ProjectToScreen({ 0.0f, 0.0f, 0.0f }, view, cx, cy));
    CHECK(wide.PickHandle(view, GizmoTool::Scale, ux, uy) == GizmoHandle::ScaleU);
    CHECK(wide.PickHandle(view, GizmoTool::Scale, cx, cy) == GizmoHandle::ScaleUniform);
    CHECK(wide.PickHandle(view, GizmoTool::Hidden, cx, cy) == GizmoHandle::None);

    // Handles that overlap on the screen: the one closer to the camera wins, whatever the order.
    GizmoPrimitive nearHandle, farHandle;
    nearHandle.handle = GizmoHandle::MoveX;
    nearHandle.shape = GizmoShape::Point;
    nearHandle.points = { { 0.0f, 0.0f, 0.0f } };      // 5 from the eye
    farHandle.handle = GizmoHandle::MoveY;
    farHandle.shape = GizmoShape::Point;
    farHandle.points = { { 0.0f, 0.0f, 2.0f } };       // 7 from the eye, same pixel
    CHECK(CutPlane::PickFromPrimitives(view, { nearHandle, farHandle }, 800.0f, 450.0f) == GizmoHandle::MoveX);
    CHECK(CutPlane::PickFromPrimitives(view, { farHandle, nearHandle }, 800.0f, 450.0f) == GizmoHandle::MoveX);
    // Same distance to the eye: the one under the cursor wins.
    GizmoPrimitive left, right;
    left.handle = GizmoHandle::RotateU;
    left.shape = GizmoShape::Point;
    left.points = { { -0.05f, 0.0f, 0.0f } };
    right.handle = GizmoHandle::RotateV;
    right.shape = GizmoShape::Point;
    right.points = { { 0.05f, 0.0f, 0.0f } };
    float lx = 0.0f, ly = 0.0f;
    CHECK(CutPlane::ProjectToScreen({ 0.05f, 0.0f, 0.0f }, view, lx, ly));
    CHECK(CutPlane::PickFromPrimitives(view, { left, right }, lx, ly, 100.0f) == GizmoHandle::RotateV);
    // A handle behind the camera is never picked.
    GizmoPrimitive behind;
    behind.handle = GizmoHandle::MoveZ;
    behind.shape = GizmoShape::Point;
    behind.points = { { 0.0f, 0.0f, -9.0f } };
    CHECK(CutPlane::PickFromPrimitives(view, { behind }, 800.0f, 450.0f, 1000.0f) == GizmoHandle::None);
}

static void TestGizmoStateMachine()
{
    const GizmoView view = MakeTestView(true);
    CutPlane plane;
    plane.SetExtentLimits(0.1f, 3.0f);
    plane.SetGizmoTool(GizmoTool::Move);
    CHECK(plane.GetGizmoTool() == GizmoTool::Move);

    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
    const std::vector<GizmoPrimitive> prims = plane.BuildGizmoPrimitives(view, GizmoTool::Move);
    CHECK(CutPlane::ProjectToScreen(prims[0].points[0], view, ax, ay) && CutPlane::ProjectToScreen(prims[0].points[1], view, bx, by));
    GizmoInput input;
    input.cursorValid = true;
    input.x = (ax + bx) * 0.5f;
    input.y = (ay + by) * 0.5f;
    const GizmoInput onArrow = input;

    // Hover only.
    CHECK(!plane.UpdateGizmo(view, input));
    CHECK(plane.GetHoveredHandle() == GizmoHandle::MoveX && !plane.IsDragging());

    // While the camera is being used nothing is hovered or started.
    input.blocked = true;
    input.leftPressed = input.leftDown = true;
    CHECK(!plane.UpdateGizmo(view, input) && plane.GetHoveredHandle() == GizmoHandle::None && !plane.IsDragging());
    input.blocked = false;

    // Press on the handle, then move the cursor along the arrow: the plane follows.
    CHECK(!plane.UpdateGizmo(view, input));
    CHECK(plane.IsDragging() && plane.GetActiveHandle() == GizmoHandle::MoveX);
    input.leftPressed = false;
    input.x += 80.0f;
    CHECK(plane.UpdateGizmo(view, input));
    CHECK(plane.GetCenter().x > 0.2f && plane.GetCenter().y == 0.0f && plane.GetCenter().z == 0.0f);

    // Outside the window the plane stays where it is; releasing ends the drag.
    const float movedX = plane.GetCenter().x;
    input.cursorValid = false;
    CHECK(!plane.UpdateGizmo(view, input) && plane.IsDragging() && plane.GetCenter().x == movedX);
    input.cursorValid = true;
    input.leftDown = false;
    CHECK(!plane.UpdateGizmo(view, input) && !plane.IsDragging() && plane.GetCenter().x == movedX);

    // Changing the tool during a drag cancels it; the hidden tool shows and picks nothing.
    CutPlane fresh;
    fresh.SetGizmoTool(GizmoTool::Move);
    input = onArrow;
    input.leftDown = input.leftPressed = true;
    fresh.UpdateGizmo(view, input);
    CHECK(fresh.IsDragging());
    fresh.SetGizmoTool(GizmoTool::Hidden);
    CHECK(!fresh.IsDragging() && fresh.GetHoveredHandle() == GizmoHandle::None);
    CHECK(!fresh.UpdateGizmo(view, input) && !fresh.IsDragging());

    // CancelGizmo ends a drag and keeps the tool.
    fresh.SetGizmoTool(GizmoTool::Move);
    fresh.UpdateGizmo(view, input);
    CHECK(fresh.IsDragging());
    fresh.CancelGizmo();
    CHECK(!fresh.IsDragging() && fresh.GetHoveredHandle() == GizmoHandle::None && fresh.GetGizmoTool() == GizmoTool::Move);

    // A drag that ends past the size limits is clamped by UpdateGizmo (the Scale tool).
    CutPlane scaler = FlatPlane(2.0f, 1.0f);
    scaler.SetExtentLimits(0.1f, 3.0f);
    scaler.SetGizmoTool(GizmoTool::Scale);
    float ux = 0.0f, uy = 0.0f;
    CHECK(CutPlane::ProjectToScreen({ 2.0f, 0.0f, 0.0f }, view, ux, uy));
    GizmoInput scaleInput;
    scaleInput.cursorValid = true;
    scaleInput.x = ux;
    scaleInput.y = uy;
    scaleInput.leftDown = scaleInput.leftPressed = true;
    scaler.UpdateGizmo(view, scaleInput);
    CHECK(scaler.GetActiveHandle() == GizmoHandle::ScaleU);
    scaleInput.leftPressed = false;
    scaleInput.x = ux + 500.0f;
    CHECK(scaler.UpdateGizmo(view, scaleInput) && scaler.GetHalfExtents().x == 3.0f);
}

static void TestGizmoBake()
{
    const GizmoView view = MakeTestView(true);
    CutPlane plane;
    plane.SetGizmoTool(GizmoTool::Hidden);
    ProceduralMeshData hidden;
    plane.BakeGizmo(view, hidden);
    CHECK(hidden.vertices.empty() && hidden.indices.empty() && hidden.sections.empty());

    for (GizmoTool tool : { GizmoTool::Move, GizmoTool::Rotate, GizmoTool::Scale })
    {
        plane.SetGizmoTool(tool);
        ProceduralMeshData data;
        plane.BakeGizmo(view, data);
        CHECK(!data.vertices.empty() && !data.indices.empty() && data.sections.size() == 1);
        ProceduralMesh mesh;
        std::string error;
        CHECK(mesh.SetMesh(data, &error));
        // Nothing is yellow unless a handle is hovered or dragged.
        bool yellow = false;
        for (const MeshRenderVertex& v : data.vertices)
            yellow = yellow || (v.color.x == 1.0f && v.color.y == 0.9f && v.color.z == 0.1f);
        CHECK(!yellow);
    }

    // Hover and drag come from the plane's own gizmo state: the cursor on the X arrow, then the button pressed.
    plane.SetGizmoTool(GizmoTool::Move);
    ProceduralMeshData plain;
    plane.BakeGizmo(view, plain);
    float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
    const std::vector<GizmoPrimitive> prims = plane.BuildGizmoPrimitives(view, GizmoTool::Move);
    CHECK(CutPlane::ProjectToScreen(prims[0].points[0], view, ax, ay) && CutPlane::ProjectToScreen(prims[0].points[1], view, bx, by));
    GizmoInput input;
    input.cursorValid = true;
    input.x = (ax + bx) * 0.5f;
    input.y = (ay + by) * 0.5f;
    plane.UpdateGizmo(view, input);
    CHECK(plane.GetHoveredHandle() == GizmoHandle::MoveX && !plane.IsDragging());
    ProceduralMeshData hovered;
    plane.BakeGizmo(view, hovered);
    input.leftDown = input.leftPressed = true;
    plane.UpdateGizmo(view, input);
    CHECK(plane.GetActiveHandle() == GizmoHandle::MoveX);
    ProceduralMeshData dragged;
    plane.BakeGizmo(view, dragged);
    const auto yellowCount = [](const ProceduralMeshData& d)
    {
        size_t count = 0;
        for (const MeshRenderVertex& v : d.vertices)
            count += (v.color.x == 1.0f && v.color.y == 0.9f && v.color.z == 0.1f);
        return count;
    };
    CHECK(yellowCount(hovered) > 0 && yellowCount(dragged) > 0 && yellowCount(plain) == 0);
    CHECK(hovered.vertices.size() == plain.vertices.size());
}

// ---------------------------------------------------------------- 2-5 tests (skinning)

typedef std::vector<std::vector<DirectX::XMFLOAT4X4>> SlotMatrices;

static DirectX::XMFLOAT4X4 ToFloat4x4(DirectX::FXMMATRIX m)
{
    DirectX::XMFLOAT4X4 out;
    DirectX::XMStoreFloat4x4(&out, m);
    return out;
}

static SlotMatrices IdentitySlots(size_t joints)
{
    SlotMatrices slots(1);
    slots[0].assign(joints, ToFloat4x4(DirectX::XMMatrixIdentity()));
    return slots;
}

// Two moderate, different rigid motions: skinning a vertex between them shears the box a little.
static SlotMatrices PoseA()
{
    SlotMatrices slots(1);
    slots[0].push_back(ToFloat4x4(DirectX::XMMatrixRotationY(0.4f) * DirectX::XMMatrixTranslation(3.0f, 1.0f, -2.0f)));
    slots[0].push_back(ToFloat4x4(DirectX::XMMatrixRotationZ(0.25f) * DirectX::XMMatrixTranslation(3.5f, 2.0f, -1.7f)));
    return slots;
}

static SlotMatrices PoseB()
{
    SlotMatrices slots(1);
    slots[0].push_back(ToFloat4x4(DirectX::XMMatrixRotationY(-0.3f) * DirectX::XMMatrixTranslation(-2.0f, 0.5f, 4.0f)));
    slots[0].push_back(ToFloat4x4(DirectX::XMMatrixRotationX(0.35f) * DirectX::XMMatrixTranslation(-1.5f, 2.5f, 4.3f)));
    return slots;
}

// A closed box skinned by joints 0 and 1: weight of joint 1 grows with height. The vertices are
// posed with `slots`, so the piece is consistent with that pose.
static MeshGeometry MakeSkinnedBox(const SlotMatrices& slots)
{
    MeshGeometry box = MakeBox();
    std::vector<SkinVertex> skinVertices = GetSkinVertices(box);
    for (size_t i = 0; i < box.vertices.size(); ++i)
    {
        SkinVertex& skin = skinVertices[i];
        skin.bindPosition = box.vertices[i].position;
        skin.jointCount = 2;
        skin.joints[0] = 0;
        skin.joints[1] = 1;
        const float w1 = box.vertices[i].position.y * 0.5f * 0.8f + 0.1f;      // 0.1 .. 0.9
        skin.weights[0] = 1.0f - w1;
        skin.weights[1] = w1;
    }
    SetSkinVertices(box, skinVertices);
    SkinStats stats;
    std::string detail;
    const bool ok = box.skin->Apply(box, slots, &stats, &detail);
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL MakeSkinnedBox: %s\n", detail.c_str());
    }
    return box;
}

static double MaxNodeDelta(const MeshGeometry& a, const MeshGeometry& b)
{
    double worst = 0.0;
    if (a.vertices.size() != b.vertices.size())
        return 1e30;
    for (size_t i = 0; i < a.vertices.size(); ++i)
    {
        const double dx = double(a.vertices[i].position.x) - b.vertices[i].position.x;
        const double dy = double(a.vertices[i].position.y) - b.vertices[i].position.y;
        const double dz = double(a.vertices[i].position.z) - b.vertices[i].position.z;
        worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return worst;
}

static bool SamePosition(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// 1. Identity matrices put every original vertex at its bind position, whatever it held before.
static void TestSkinIdentity()
{
    MeshGeometry box = MakeBox();
    const MeshGeometry reference = box;
    for (MeshVertex& vertex : box.vertices)
        vertex.position = { vertex.position.x + 100.0f, -7.0f, 3.0f };
    SkinStats stats;
    std::string detail;
    CHECK(box.skin->Apply(box, IdentitySlots(1), &stats, &detail));
    CHECK(MaxNodeDelta(box, reference) == 0.0);
    CHECK(stats.skinVerticesEvaluated == 8 && stats.surfaceVerticesWritten == 8 && stats.capVerticesSkipped == 0);
}

// 2. One joint with a rotation and a translation: bind * R * T, row-vector convention.
static void TestSkinSingleJoint()
{
    MeshGeometry tri = MakePiece({ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { { 0, 1, 2 } });
    std::vector<SkinVertex> triSkin = GetSkinVertices(tri);
    for (SkinVertex& skin : triSkin)
        skin.joints[0] = 1;
    SetSkinVertices(tri, triSkin);
    SlotMatrices slots = IdentitySlots(2);
    slots[0][1] = ToFloat4x4(DirectX::XMMatrixRotationZ(1.5707963267948966f) * DirectX::XMMatrixTranslation(5.0f, 0.0f, 0.0f));
    std::string detail;
    CHECK(tri.skin->Apply(tri, slots, nullptr, &detail));
    const XMFLOAT3 expected[3] = { { 5, 1, 0 }, { 4, 0, 0 }, { 5, 0, 1 } };    // (1,0,0)->(0,1,0)+5x, ...
    for (int i = 0; i < 3; ++i)
        CHECK(std::fabs(tri.vertices[i].position.x - expected[i].x) < 1e-5f && std::fabs(tri.vertices[i].position.y - expected[i].y) < 1e-5f
            && std::fabs(tri.vertices[i].position.z - expected[i].z) < 1e-5f);
}

// 3. Two joints with weights 0.25 / 0.75, computed by hand:
//    0.25 * (5,1,1) + 0.75 * (1,9,1) = (2,7,1).
static void TestSkinTwoJointBlend()
{
    MeshGeometry tri = MakePiece({ { 1, 1, 1 }, { 2, 2, 2 }, { 3, 3, 3 } }, { { 0, 1, 2 } });
    std::vector<SkinVertex> triSkin = GetSkinVertices(tri);
    SkinVertex& skin = triSkin[0];
    skin.jointCount = 2;
    skin.joints[0] = 0;
    skin.joints[1] = 1;
    skin.weights[0] = 0.25f;
    skin.weights[1] = 0.75f;
    SetSkinVertices(tri, triSkin);
    SlotMatrices slots(1);
    slots[0].push_back(ToFloat4x4(DirectX::XMMatrixTranslation(4.0f, 0.0f, 0.0f)));
    slots[0].push_back(ToFloat4x4(DirectX::XMMatrixTranslation(0.0f, 8.0f, 0.0f)));
    CHECK(tri.skin->Apply(tri, slots));
    CHECK(std::fabs(tri.vertices[0].position.x - 2.0f) < 1e-6f && std::fabs(tri.vertices[0].position.y - 7.0f) < 1e-6f
        && std::fabs(tri.vertices[0].position.z - 1.0f) < 1e-6f);
    // The same blend straight from SkinPoint.
    XMFLOAT3 point;
    DirectX::XMStoreFloat3(&point, Skinning::SkinPoint(slots[0].data(), 2, skin.joints, skin.weights, 2, skin.bindPosition));
    CHECK(point.x == tri.vertices[0].position.x && point.y == tri.vertices[0].position.y && point.z == tri.vertices[0].position.z);
}

// 4. A piece cut in a pose and skinned with the same matrices again lands on the cut positions:
//    cut vertices are driven by their MeshVertexOrigin, with no joint or weight of their own.
static void TestSkinMatchesSliceNodes()
{
    const SlotMatrices pose = PoseA();
    const MeshGeometry box = MakeSkinnedBox(pose);
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 2.0f, 0, 0.3f, 1.0f, 0.2f), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    for (MeshGeometry* piece : { &result.positive, &result.negative })
    {
        MeshGeometry skinned = *piece;
        for (MeshVertex& vertex : skinned.vertices)
            vertex.position = { 0.0f, 0.0f, 0.0f };
        SkinStats stats;
        std::string detail;
        CHECK(skinned.skin->Apply(skinned, pose, &stats, &detail));
        const double tolerance = 1e-5 * ComputePieceRadius(*piece);
        CHECK(MaxNodeDelta(skinned, *piece) <= tolerance);
        CHECK(stats.capVerticesSkipped == 0);
        CHECK(stats.skinVerticesEvaluated <= box.skin->GetVertices().size());
    }
}

// 5. Seam corners and cap rim corners follow their vertex: after a pose change every corner of a vertex
//    bakes to the same bits. Cap faces use no vertices of their own.
static void TestSkinKeepsSeamsAndCapsTogether()
{
    MeshGeometry tube;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildMeshGeometry(MakeTubeMeshData(8, true, 0), kDefaultEpsilonScale, tube, stats, &detail), SliceStatus::Ok, detail);
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(tube, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);

    for (MeshGeometry* piece : { &result.positive, &result.negative })
    {
        for (const MeshVertex& vertex : piece->vertices)
            CHECK(vertex.kind == MeshVertexKind::Surface);          // no cap-only vertices exist
        ProceduralMeshData baked;
        BuildRenderMesh(*piece, baked);
        const SlotMatrices pose = PoseA();
        SkinStats skinStats;
        CHECK(piece->skin->Apply(*piece, pose, &skinStats, &detail));
        CHECK(skinStats.capVerticesSkipped == 0);
        CHECK(UpdateRenderPositions(*piece, baked.vertices));

        std::vector<int> firstCorner(piece->vertices.size(), -1);
        bool seamSeen = false, capSeen = false;
        for (size_t c = 0; c < piece->corners.size(); ++c)
        {
            const uint32_t vertex = piece->corners[c].vertex;
            if (firstCorner[vertex] < 0)
            {
                firstCorner[vertex] = int(c);
                continue;
            }
            seamSeen = true;                                // a vertex with more than one corner (UV seam or cap)
            CHECK(SamePosition(baked.vertices[c].position, baked.vertices[firstCorner[vertex]].position));
            CHECK(SamePosition(baked.vertices[c].position, piece->vertices[vertex].position));
        }
        for (const MeshFace& face : piece->faces)
            capSeen = capSeen || face.kind == MeshFaceKind::Cap;
        CHECK(seamSeen && capSeen);
    }
}

// 6. A piece with its own skin state (separated bones) is refused and left alone.
static void TestSkinRefusesSkinState()
{
    MeshGeometry box = MakeBox();
    box.skinStateId = 3;
    const MeshGeometry before = box;
    std::string detail;
    CHECK(!box.skin->Apply(box, IdentitySlots(1), nullptr, &detail));
    CHECK(detail.find("skin state") != std::string::npos);
    CHECK(MaxNodeDelta(box, before) == 0.0);
}

// 7. Cutting a piece twice: the second generation skins back to its own positions as well.
static void TestSkinAfterTwoCuts()
{
    const SlotMatrices pose = PoseA();
    const MeshGeometry box = MakeSkinnedBox(pose);
    double yLow = 1e30, yHigh = -1e30;
    for (const MeshVertex& vertex : box.vertices)
    {
        yLow = std::min<double>(yLow, vertex.position.y);
        yHigh = std::max<double>(yHigh, vertex.position.y);
    }
    GeometrySliceResult first, second;
    const float yMid = float((yLow + yHigh) * 0.5);
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, yMid, 0, 0, 1, 0), 1, CapOptions(), first), SliceStatus::Ok, first.detail);
    // Cut the upper half again, a quarter of the way up. Only the surface is crossed.
    double upLow = 1e30, upHigh = -1e30;
    for (const MeshVertex& vertex : first.positive.vertices)
    {
        upLow = std::min<double>(upLow, vertex.position.y);
        upHigh = std::max<double>(upHigh, vertex.position.y);
    }
    const float yQuarter = float(upLow + (upHigh - upLow) * 0.4);
    CHECK_STATUS(SliceGeometry(first.positive, MakePlane(0, yQuarter, 0, 0, 1, 0), 2, CapOptions(), second), SliceStatus::Ok, second.detail);
    for (MeshGeometry* piece : { &second.positive, &second.negative })
    {
        MeshGeometry skinned = *piece;
        for (MeshVertex& vertex : skinned.vertices)
            vertex.position = { 9.0f, 9.0f, 9.0f };
        std::string detail;
        CHECK(skinned.skin->Apply(skinned, pose, nullptr, &detail));
        CHECK(MaxNodeDelta(skinned, *piece) <= 1e-5 * ComputePieceRadius(*piece));
    }
    // 두 번째 세대의 절단 정점은 스킨 정점 최대 3개에 의존한다.
    for (const MeshVertex& vertex : second.positive.vertices)
        CHECK(vertex.origin.skinVertex[0] != kInvalidIndex);
    // New pose: the second-generation piece moves with the same matrices.
    MeshGeometry moved = second.positive;
    CHECK(moved.skin->Apply(moved, PoseB()));
    CHECK(MaxNodeDelta(moved, second.positive) > 0.1);
}

// Bad input is refused without touching the piece.
// 용접이 만든 skin은 자른 두 piece가 같은 객체로 공유하고, skin이 없는 piece는 스킨 정점이 없는 표로 검증한다.
static void TestSkinningShared()
{
    MeshGeometry box;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildMeshGeometry(MakeBoxMeshData(), kDefaultEpsilonScale, box, stats, &detail), SliceStatus::Ok, detail);
    CHECK(box.skin != nullptr && box.skin->GetVertices().size() == 8);

    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.positive.skin == box.skin && result.negative.skin == box.skin);
    CHECK(box.skin.use_count() >= 3);

    // skin이 없으면 Surface 정점의 origin이 범위 밖이라 InvalidOrigin이다.
    MeshGeometry noSkin = box;
    noSkin.skin.reset();
    CHECK_STATUS(ValidateMeshGeometry(noSkin, kDefaultEpsilonScale, &detail), SliceStatus::InvalidOrigin, detail);
    CHECK(box.skin != nullptr);
}

static void TestSkinRefusesBadInput()
{
    MeshGeometry box = MakeSkinnedBox(PoseA());
    const MeshGeometry before = box;
    std::string detail;
    CHECK(!box.skin->Apply(box, IdentitySlots(1), nullptr, &detail));           // joint 1 has no matrix
    CHECK(detail.find("joint") != std::string::npos && MaxNodeDelta(box, before) == 0.0);

    CHECK(!box.skin->Apply(box, SlotMatrices(), nullptr, &detail));             // no slots at all
    CHECK(detail.find("slot") != std::string::npos && MaxNodeDelta(box, before) == 0.0);

    MeshGeometry badOrigin = before;
    badOrigin.vertices[2].origin.skinVertex[0] = 99;
    CHECK(!badOrigin.skin->Apply(badOrigin, PoseA(), nullptr, &detail));
    CHECK(detail.find("skin vertex") != std::string::npos && MaxNodeDelta(badOrigin, before) == 0.0);
}

// ---------------------------------------------------------------- rigid (static mesh) input (U-3a)

// 스킨 없는 메시 복사본처럼: 가중치 0, 바인드 = 로컬 위치, 위치 = world × 로컬. 정점 0을 UV만 다르게 한 번 더 넣는다.
static ProceduralMeshData MakeRigidBoxMeshData(const DirectX::XMFLOAT4X4& world)
{
    ProceduralMeshData in = MakeBoxMeshData();
    for (size_t i = 0; i < in.vertices.size(); ++i)
    {
        in.bindings[i].weights[0] = 0.0f;
        DirectX::XMStoreFloat3(&in.vertices[i].position, Skinning::RigidPoint(world, in.bindings[i].bindPosition));
    }
    MeshRenderVertex twin = in.vertices[0];
    twin.uv = { 0.5f, 0.5f };
    in.vertices.push_back(twin);
    in.bindings.push_back(in.bindings[0]);
    in.indices[0] = uint32_t(in.vertices.size() - 1);      // 면 0이 쌍둥이 정점을 쓴다
    return in;
}

static void TestRigidWeldAndSkin()
{
    const DirectX::XMFLOAT4X4 world = ToFloat4x4(DirectX::XMMatrixRotationY(0.4f) * DirectX::XMMatrixTranslation(3.0f, 1.0f, -2.0f));
    const ProceduralMeshData in = MakeRigidBoxMeshData(world);

    // 가중치 0만 있는 입력이 용접을 통과하고, 같은 위치의 두 정점은 노드 하나가 된다
    MeshGeometry piece;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildMeshGeometry(in, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::Ok, detail);
    CHECK(piece.vertices.size() == 8 && piece.corners.size() == 9 && stats.boundaryEdges == 0);
    CHECK(piece.corners[8].vertex == piece.corners[0].vertex);
    for (const SkinVertex& skin : GetSkinVertices(piece))
        CHECK(skin.jointCount == 0);
    CheckPieceValid(piece, "rigid box");

    // 가중치 0이 일부만 섞인 입력은 지금처럼 거절한다
    ProceduralMeshData mixed = MakeBoxMeshData();
    mixed.bindings[3].weights[0] = 0.0f;
    MeshGeometry rejected;
    CHECK_STATUS(BuildMeshGeometry(mixed, kDefaultEpsilonScale, rejected, stats, &detail), SliceStatus::InvalidInput, detail);
    CHECK(detail.find("vertex 3") != std::string::npos);

    // 강체 정점은 행렬 하나 × 바인드 위치로 옮겨진다
    const DirectX::XMFLOAT4X4 moved = ToFloat4x4(DirectX::XMMatrixRotationX(-0.3f) * DirectX::XMMatrixTranslation(-1.0f, 0.5f, 4.0f));
    SlotMatrices slots(1, std::vector<DirectX::XMFLOAT4X4>(1, moved));
    SkinStats skinStats;
    CHECK(piece.skin->Apply(piece, slots, &skinStats, &detail));
    CHECK(skinStats.skinVerticesEvaluated == 8);
    const DirectX::XMMATRIX m = DirectX::XMLoadFloat4x4(&moved);
    const std::vector<SkinVertex> rigidSkin = GetSkinVertices(piece);
    for (size_t n = 0; n < piece.vertices.size(); ++n)
    {
        const XMFLOAT3& bind = rigidSkin[n].bindPosition;
        XMFLOAT3 expected;
        DirectX::XMStoreFloat3(&expected, DirectX::XMVector3TransformCoord(DirectX::XMLoadFloat3(&bind), m));
        CHECK(std::fabs(piece.vertices[n].position.x - expected.x) < 1e-5f && std::fabs(piece.vertices[n].position.y - expected.y) < 1e-5f
            && std::fabs(piece.vertices[n].position.z - expected.z) < 1e-5f);
    }

    // 행렬이 없는 슬롯은 실패하고 piece를 바꾸지 않는다
    const MeshGeometry before = piece;
    SlotMatrices empty(1);
    CHECK(!piece.skin->Apply(piece, empty, nullptr, &detail));
    CHECK(detail.find("rigid") != std::string::npos && MaxNodeDelta(piece, before) == 0.0);

    // 기존 스킨 piece는 SkinPoint 그대로다 (비트 단위)
    const MeshGeometry skinned = MakeSkinnedBox(PoseA());
    const SlotMatrices pose = PoseA();
    const std::vector<SkinVertex> skinnedSkin = GetSkinVertices(skinned);
    for (size_t n = 0; n < skinned.vertices.size(); ++n)
    {
        const SkinVertex& skin = skinnedSkin[n];
        XMFLOAT3 expected;
        DirectX::XMStoreFloat3(&expected, Skinning::SkinPoint(pose[0].data(), uint32_t(pose[0].size()), skin.joints, skin.weights,
            skin.jointCount, skin.bindPosition));
        CHECK(SamePosition(skinned.vertices[n].position, expected));
    }
}

// 강체 piece를 SliceProceduralMesh로 자르고, 두 조각을 행렬 하나로 함께 옮긴다
// Exercise the public mesh API while keeping geometry fixtures convenient for algorithm tests.
static bool SliceTestGeometry(MeshGeometry& geometry, const XMFLOAT3& point, const XMFLOAT3& normal,
    bool createOther, MeshGeometry* other, SliceCapOption cap, uint32_t material,
    SliceStatus* status = nullptr, std::string* error = nullptr, std::vector<std::vector<uint32_t>>* loops = nullptr)
{
    ProceduralMesh mesh, half;
    if (!mesh.SetTopology(geometry, error))
    {
        if (status) *status = SliceStatus::InvalidInput;
        return false;
    }
    if (!SliceProceduralMesh(mesh, point, normal, createOther, other ? &half : nullptr, cap, material, status, error, loops))
        return false;
    geometry = mesh.GetGeometry();
    if (other) *other = half.GetGeometry();
    return true;
}

static void TestRigidSlice()
{
    const DirectX::XMFLOAT4X4 identity = ToFloat4x4(DirectX::XMMatrixIdentity());
    MeshGeometry piece, other;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildMeshGeometry(MakeRigidBoxMeshData(identity), kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::Ok, detail);
    SliceStatus status = SliceStatus::InvalidInput;
    CHECK(SliceTestGeometry(piece, { 0, 0.7f, 0 }, { 0.2f, 1, 0.1f }, true, &other, SliceCapOption::CreateNewSectionForCap, 1,
        &status, &detail));
    CHECK(status == SliceStatus::Ok);
    CheckPieceValid(piece, "rigid +");
    CheckPieceValid(other, "rigid -");
    CHECK(CountCapFaces(piece) > 0 && CountCapFaces(other) > 0);

    // 표면 노드(절단 노드 포함)는 정확히 이동량만큼 움직이고, cap 노드는 Apply가 건드리지 않는다
    const SlotMatrices slots(1, std::vector<DirectX::XMFLOAT4X4>(1, ToFloat4x4(DirectX::XMMatrixTranslation(2.0f, -1.0f, 0.5f))));
    for (MeshGeometry* half : { &piece, &other })
    {
        const MeshGeometry before = *half;
        CHECK(half->skin->Apply(*half, slots, nullptr, &detail));
        for (size_t n = 0; n < half->vertices.size(); ++n)
        {
            if (half->vertices[n].kind != MeshVertexKind::Surface)
                continue;
            const XMFLOAT3 &a = before.vertices[n].position, &b = half->vertices[n].position;
            CHECK(std::fabs(b.x - a.x - 2.0f) < 1e-5f && std::fabs(b.y - a.y + 1.0f) < 1e-5f && std::fabs(b.z - a.z - 0.5f) < 1e-5f);
        }
    }
}

// 윗면이 없는 강체 상자(버니 바닥 구멍과 같은 상황): 열린 경계는 OpenSurface이고, 구멍을 지나는 세로 절단은
// cap 없이 성공하며(Unreal 방식), 구멍을 지나지 않는 가로 절단은 지금처럼 cap을 만든다.
static void TestRigidOpenBoxSlice()
{
    ProceduralMeshData data = MakeRigidBoxMeshData(ToFloat4x4(DirectX::XMMatrixIdentity()));
    data.indices.erase(data.indices.begin() + 6, data.indices.begin() + 12);       // 면 2, 3 = 윗면 (y = 2)
    MeshGeometry box;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildMeshGeometry(data, kDefaultEpsilonScale, box, stats, &detail), SliceStatus::Ok, detail);
    CHECK(stats.openSurfaceEdges == 4 && stats.replacementEdges == 0);
    CHECK(CountKind(box, MeshEdgeTag::OpenSurface) == 4);

    MeshGeometry piece = box, other;
    SliceStatus status = SliceStatus::InvalidInput;
    std::vector<std::vector<uint32_t>> loops;
    CHECK(SliceTestGeometry(piece, { 0.3f, 0, 0 }, { 1, 0, 0.2f }, true, &other, SliceCapOption::CreateNewSectionForCap, 1,
        &status, &detail, &loops));
    CHECK(status == SliceStatus::Ok);
    CHECK(loops.size() == 1);       // 열린 사슬 하나 (유한 평면 검사용으로 outLoops에 붙는다)
    CHECK(CountCapFaces(piece) == 0 && CountCapFaces(other) == 0);
    CheckPieceValid(piece, "open box +");
    CheckPieceValid(other, "open box -");

    piece = box;
    CHECK(SliceTestGeometry(piece, { 0, 1, 0 }, { 0, 1, 0 }, true, &other, SliceCapOption::CreateNewSectionForCap, 1,
        &status, &detail, &loops));
    CHECK(status == SliceStatus::Ok && loops.size() == 1);
    CHECK(CountCapFaces(piece) > 0 && CountCapFaces(other) > 0);

    // 스킨 입력의 열린 경계는 여전히 Replacement이고, 거기에 닿는 절단은 OpenLoop다
    ProceduralMeshData skinned = MakeBoxMeshData();
    skinned.indices.erase(skinned.indices.begin() + 6, skinned.indices.begin() + 12);
    MeshGeometry patch;
    CHECK_STATUS(BuildMeshGeometry(skinned, kDefaultEpsilonScale, patch, stats, &detail), SliceStatus::Ok, detail);
    CHECK(stats.replacementEdges == 4 && stats.openSurfaceEdges == 0);
    GeometrySliceResult result;
    CHECK_STATUS(SliceGeometry(patch, MakePlane(0.3f, 0, 0, 1, 0, 0.2f), 1, SliceOptions(), result), SliceStatus::OpenLoop, result.detail);
}

// ---------------------------------------------------------------- main

// ---------------------------------------------------------------- cut job tests (Runtime/CutJob.h)

static CutJob CandidateJob(const CutJobKey& key)
{
    CutJob job;
    job.key = key;
    job.candidate = true;
    return job;
}

static std::map<CutJobKey, CutJob> MakeJobs(const std::vector<CutJobKey>& candidates, const std::vector<CutJobKey>& confirmedOnly)
{
    std::map<CutJobKey, CutJob> jobs;
    for (const CutJobKey& key : candidates)
        jobs[key] = CandidateJob(key);
    for (const CutJobKey& key : confirmedOnly)
    {
        CutJob job;
        job.key = key;
        job.haveConfirmed = true;
        jobs[key] = job;
    }
    return jobs;
}

static void TestProceduralMeshOwnsGeometry()
{
    MeshGeometry box;
    WeldStats stats;
    std::string error;
    CHECK_STATUS(BuildMeshGeometry(MakeBoxMeshData(), kDefaultEpsilonScale, box, stats, &error), SliceStatus::Ok, error);
    GeometrySliceResult cut;
    CHECK_STATUS(SliceGeometry(box, MakePlane(0, 0.5f, 0, 0, 1, 0), 1, CapOptions(), cut), SliceStatus::Ok, cut.detail);
    ProceduralMesh mesh;
    CHECK(mesh.SetTopology(std::move(cut.positive), &error));
    MeshGeometry expected = mesh.GetGeometry();
    const auto sharedSkin = expected.skin;
    const uint64_t topology = mesh.GetTopologyVersion();
    const uint64_t beforePose = mesh.GetVertexVersion();
    SlotMatrices slots(1, std::vector<XMFLOAT4X4>(1, ToFloat4x4(XMMatrixTranslation(3, 1, -2))));
    CHECK(expected.skin->Apply(expected, slots, nullptr, &error));
    CHECK(mesh.UpdatePose(slots, &error));
    CHECK(mesh.GetGeometry().vertices[0].position.x == expected.vertices[0].position.x);
    CHECK(mesh.GetVertexVersion() > beforePose && mesh.GetTopologyVersion() == topology);
    CHECK(mesh.GetGeometry().skin == sharedSkin);
    for (size_t i = 0; i < expected.corners.size(); ++i)
    {
        const XMFLOAT3& p = expected.vertices[expected.corners[i].vertex].position;
        const XMFLOAT3& actual = mesh.GetData().vertices[i].position;
        CHECK(actual.x == p.x && actual.y == p.y && actual.z == p.z);
    }
    const uint64_t version = mesh.GetVertexVersion();
    const float x = mesh.GetGeometry().vertices[0].position.x;
    CHECK(!mesh.UpdatePose({}, &error));
    const float largest = std::numeric_limits<float>::max();
    SlotMatrices huge(1, std::vector<XMFLOAT4X4>(1, ToFloat4x4(XMMatrixScaling(largest, 1, largest))));
    CHECK(!mesh.UpdatePose(huge, &error));   // finite points, but an unrepresentable render bounds radius
    CHECK(!mesh.UpdateVertices(mesh.GetData().vertices, &error));
    MeshGeometry invalid = mesh.GetGeometry();
    invalid.faces[0].corner[0] = kInvalidIndex;
    CHECK(!mesh.SetTopology(std::move(invalid), &error));
    CHECK(mesh.GetVertexVersion() == version && mesh.GetGeometry().vertices[0].position.x == x);
    CHECK(mesh.SetSectionSplit(true, &error));
    RenderBakeOptions options;
    options.splitByOrigin = true;
    ProceduralMeshData baked;
    BuildRenderMesh(expected, baked, options);
    CHECK(mesh.GetData().indices == baked.indices && mesh.GetData().sections.size() == baked.sections.size());
    const uint64_t splitTopology = mesh.GetTopologyVersion();
    CHECK(splitTopology > topology && mesh.SetSectionSplit(true, &error));
    CHECK(mesh.GetTopologyVersion() == splitTopology);
    CHECK(mesh.SetSectionSplit(false, &error));
    BuildRenderMesh(expected, baked);
    CHECK(mesh.GetData().indices == baked.indices && mesh.GetData().sections.size() == baked.sections.size());
    mesh.Clear();
    CHECK(mesh.GetGeometry().Empty() && mesh.GetData().vertices.empty());
    CHECK(!mesh.UpdatePose(slots, &error));
    CHECK(mesh.SetTopology(std::move(expected), &error));
    CHECK(mesh.SetMesh(std::move(baked), &error));
    CHECK(mesh.GetGeometry().Empty());
}

static void TestPublicProceduralMeshPipeline()
{
    ProceduralMesh mesh, other;
    WeldStats stats;
    std::string error;
    CHECK_STATUS(mesh.Build(MakeBoxMeshData(), stats, &error), SliceStatus::Ok, error);
    const auto skin = mesh.GetGeometry().skin;
    const SlotMatrices identity(1, std::vector<XMFLOAT4X4>(1, ToFloat4x4(XMMatrixIdentity())));
    CHECK(mesh.UpdatePose(identity, &error));
    const double originalArea = TotalArea(mesh.GetGeometry(), MeshFaceKind::Surface);
    SliceStatus status;
    std::vector<std::vector<uint32_t>> loops;
    CHECK(SliceProceduralMesh(mesh, { 0, 0.5f, 0 }, { 0, 1, 0 }, true, &other,
        SliceCapOption::CreateNewSectionForCap, 7, &status, &error, &loops));
    CHECK(status == SliceStatus::Ok && !loops.empty());
    CHECK(mesh.GetGeometry().skin == skin && other.GetGeometry().skin == skin);
    CHECK(std::fabs(TotalArea(mesh.GetGeometry(), MeshFaceKind::Surface)
        + TotalArea(other.GetGeometry(), MeshFaceKind::Surface) - originalArea) < 1e-5);
    for (ProceduralMesh* half : { &mesh, &other })
    {
        CHECK(!half->GetData().indices.empty() && half->GetBounds().valid);
        CHECK(half->GetGeometry().caps.back().materialSlot == 7);
        CHECK(half->UpdatePose(identity, &error));
    }
    const uint64_t version = mesh.GetVertexVersion(), otherVersion = other.GetVertexVersion();
    CHECK(!SliceProceduralMesh(mesh, { 0, 0, 0 }, { 0, 0, 0 }, true, &other,
        SliceCapOption::NoCap, 1, &status, &error, &loops));
    CHECK(status == SliceStatus::InvalidInput && mesh.GetVertexVersion() == version
        && other.GetVertexVersion() == otherVersion && !loops.empty());
    CHECK(!SliceProceduralMesh(mesh, { 0, 0.75f, 0 }, { 0, 1, 0 }, true, &mesh,
        SliceCapOption::NoCap, 1, &status, &error));
    ProceduralMeshData bad = MakeBoxMeshData();
    bad.bindings.clear();
    CHECK_STATUS(mesh.Build(bad, stats, &error), SliceStatus::InvalidInput, error);
    CHECK(mesh.GetVertexVersion() == version);
}

static void TestCutJobKey()
{
    // 같은 부모의 형제 선분은 자식 joint로 구분된다.
    const CutJobKey torsoNeck = MakeSegmentKey(1, 0, 2, 3);
    const CutJobKey torsoLeftArm = MakeSegmentKey(1, 0, 2, 5);
    const CutJobKey torsoRightArm = MakeSegmentKey(1, 0, 2, 6);
    CHECK(!(torsoNeck == torsoLeftArm) && !(torsoLeftArm == torsoRightArm));
    CHECK(torsoNeck < torsoLeftArm && torsoLeftArm < torsoRightArm && !(torsoRightArm < torsoNeck));
    CHECK(torsoNeck == MakeSegmentKey(1, 0, 2, 3) && !(torsoNeck < MakeSegmentKey(1, 0, 2, 3)));

    // 대상, 팔레트, 부모 joint가 다르면 다른 작업이다. 대상이 가장 먼저 비교된다.
    CHECK(!(MakeSegmentKey(1, 0, 2, 3) == MakeSegmentKey(2, 0, 2, 3)));
    CHECK(MakeSegmentKey(1, 9, 9, 9) < MakeSegmentKey(2, 0, 0, 0));
    CHECK(MakeSegmentKey(1, 0, 9, 9) < MakeSegmentKey(1, 1, 0, 0));
    CHECK(MakeSegmentKey(1, 0, 2, 9) < MakeSegmentKey(1, 0, 3, 0));
    CHECK(!(MakeSegmentKey(1, 0, 2, 3) == MakeSegmentKey(1, 1, 2, 3)) && !(MakeSegmentKey(1, 0, 2, 3) == MakeSegmentKey(1, 0, 4, 3)));

    // 종류가 다르면 다른 작업이고, 스태틱 메시 key는 메시 번호만 쓴다.
    const CutJobKey mesh = MakeStaticMeshKey(1, 4);
    CHECK(mesh.kind == CutJobKind::StaticMesh && mesh.a == 4 && mesh.b == kInvalidIndex && mesh.c == kInvalidIndex);
    CHECK(!(mesh == MakeSegmentKey(1, 4, kInvalidIndex, kInvalidIndex)));
    CHECK(MakeSegmentKey(1, 9, 9, 9) < mesh);
    CHECK(mesh == MakeStaticMeshKey(1, 4) && !(mesh == MakeStaticMeshKey(1, 5)) && !(mesh == MakeStaticMeshKey(2, 4)));

    // std::map에서 같은 key는 하나로 합쳐지고 순서는 사전식이다.
    std::map<CutJobKey, int> ordered;
    ordered[torsoRightArm] = 3;
    ordered[torsoNeck] = 1;
    ordered[torsoLeftArm] = 2;
    ordered[MakeSegmentKey(1, 0, 2, 3)] = 10;
    CHECK(ordered.size() == 3 && ordered.begin()->first == torsoNeck && ordered.begin()->second == 10);
    CHECK(ordered.rbegin()->first == torsoRightArm);

    // joint key: 팔레트와 joint만 쓰고, 선분 key와 종류로 구분되며, 같은 joint의 형제 선분은 하나로 합쳐진다.
    const CutJobKey torso = MakeJointKey(1, 0, 2);
    CHECK(torso.kind == CutJobKind::SkeletalJoint && torso.a == 0 && torso.b == 2 && torso.c == kInvalidIndex);
    CHECK(torso == MakeJointKey(1, 0, 2) && !(torso == MakeJointKey(1, 0, 3)) && !(torso == MakeJointKey(1, 1, 2)) && !(torso == MakeJointKey(2, 0, 2)));
    CHECK(!(torso == MakeSegmentKey(1, 0, 2, kInvalidIndex)) && torso < MakeJointKey(1, 0, 3) && torso < mesh && MakeSegmentKey(1, 9, 9, 9) < torso);
}

static void TestDiffJobs()
{
    const CutJobKey a = MakeSegmentKey(1, 0, 2, 3), b = MakeSegmentKey(1, 0, 2, 5), c = MakeSegmentKey(1, 0, 4, 5);
    const CutJobKey d = MakeSegmentKey(2, 0, 1, 2);

    // 작업이 하나도 없으면 이번 key가 모두 새 작업이다 (들어온 순서).
    CutJobDiff diff = DiffJobs(MakeJobs({}, {}), { c, a });
    CHECK(diff.added.size() == 2 && diff.added[0] == c && diff.added[1] == a && diff.lost.empty() && diff.kept.empty());

    // 계속 후보인 작업은 kept, 새 key는 added, 사라진 후보는 lost.
    diff = DiffJobs(MakeJobs({ a, b }, {}), { b, c });
    CHECK(diff.kept.size() == 1 && diff.kept[0] == b);
    CHECK(diff.added.size() == 1 && diff.added[0] == c);
    CHECK(diff.lost.size() == 1 && diff.lost[0] == a);

    // 확정 결과만 남은 작업은 후보가 아니므로 lost가 아니다. 다시 나타나면 패치를 다시 만들어야 해서 added다.
    diff = DiffJobs(MakeJobs({ a }, { b }), { a });
    CHECK(diff.kept.size() == 1 && diff.lost.empty() && diff.added.empty());
    diff = DiffJobs(MakeJobs({ a }, { b }), { a, b });
    CHECK(diff.kept.size() == 1 && diff.kept[0] == a && diff.added.size() == 1 && diff.added[0] == b && diff.lost.empty());

    // 교차 선분이 하나도 없으면 후보였던 작업이 모두 lost다 (key 순서).
    diff = DiffJobs(MakeJobs({ d, b, a }, { c }), {});
    CHECK(diff.lost.size() == 3 && diff.lost[0] == a && diff.lost[1] == b && diff.lost[2] == d && diff.added.empty() && diff.kept.empty());

    // 같은 key가 두 번 들어와도 한 번만 센다. 대상이 다르면 다른 key다.
    diff = DiffJobs(MakeJobs({ a }, {}), { a, a, d, d });
    CHECK(diff.kept.size() == 1 && diff.added.size() == 1 && diff.added[0] == d && diff.lost.empty());
}

static void TestJobColors()
{
    // 8색이 돌아가며 쓰이고, 이웃한 번호의 색은 서로 다르다.
    for (uint32_t i = 0; i < kJobColorCount; ++i)
    {
        const XMFLOAT4 color = GetJobColor(i);
        const XMFLOAT4 next = GetJobColor(i + 1);
        CHECK(color.w == 1.0f && (color.x != next.x || color.y != next.y || color.z != next.z));
        CHECK(GetJobColor(i + kJobColorCount).x == color.x && GetJobColor(i + 5 * kJobColorCount).z == color.z);
    }
    // 8색 모두 서로 다르다.
    for (uint32_t i = 0; i < kJobColorCount; ++i)
        for (uint32_t j = i + 1; j < kJobColorCount; ++j)
        {
            const XMFLOAT4 p = GetJobColor(i), q = GetJobColor(j);
            CHECK(p.x != q.x || p.y != q.y || p.z != q.z);
        }
    // 작업이 만들어질 때 기본 상태.
    const CutJob job{};
    CHECK(!job.candidate && !job.haveConfirmed && job.colorIndex == 0 && job.source == nullptr);
}


// ---------------------------------------------------------------- cut pick tests (Geometry/CutPick.h)

static SkinVertex MakeRigidSkin(uint32_t joint)
{
    SkinVertex skin;
    skin.jointCount = 1;
    skin.joints[0] = uint16_t(joint);
    skin.weights[0] = 1.0f;
    return skin;
}

// 높이 0..2의 사각 기둥 옆면 (위·아래 면은 없다). 아래 고리 정점 0~3, 위 고리 정점 4~7, 옆면 삼각형 8개.
static CutPickMesh MakePrismMesh(uint32_t bottomJoint, uint32_t topJoint)
{
    CutPickMesh mesh;
    const float xs[4] = { -1.0f, 1.0f, 1.0f, -1.0f }, zs[4] = { -1.0f, -1.0f, 1.0f, 1.0f };
    for (int ring = 0; ring < 2; ++ring)
        for (int i = 0; i < 4; ++i)
        {
            mesh.positions.push_back({ xs[i], ring == 0 ? 0.0f : 2.0f, zs[i] });
            mesh.skins.push_back(MakeRigidSkin(ring == 0 ? bottomJoint : topJoint));
        }
    for (uint32_t i = 0; i < 4; ++i)
    {
        const uint32_t j = (i + 1) % 4;
        const uint32_t quad[6] = { i, j, 4 + j, i, 4 + j, 4 + i };
        mesh.indices.insert(mesh.indices.end(), quad, quad + 6);
    }
    return mesh;
}

static std::vector<CutPickJoint> Pick(const CutPickMesh& mesh, const std::vector<uint32_t>& parents, const CutPlane& plane,
    float minWeight = 0.05f, float epsilon = 0.0f)
{
    std::vector<CutPickJoint> out;
    CutPick::PickJoints(mesh, parents, plane, minWeight, epsilon, out);
    return out;
}

static void TestPickJointsSingleJoint()
{
    // 한 joint로만 스키닝된 기둥을 가로로 자르면 절단 joint 하나에 옆면 삼각형 8개가 모두 모인다.
    const CutPickMesh prism = MakePrismMesh(1, 1);
    const std::vector<CutPickJoint> picked = Pick(prism, { kInvalidIndex, 0 }, HorizontalPlane(0, 1, 0, 5.0f));
    CHECK(picked.size() == 1 && picked[0].joint == 1 && picked[0].triangleCount == 8);

    // 계측 개수: 옆면 삼각형 8개를 모두 훑고, 모두 평면 양쪽에 걸쳤고, 모두 교차로 셌다. 사각형이 일부만 덮으면 교차만 줄어든다.
    CutPickStats stats;
    std::vector<CutPickJoint> counted;
    CutPick::PickJoints(prism, { kInvalidIndex, 0 }, HorizontalPlane(0, 1, 0, 5.0f), 0.05f, 0.0f, counted, &stats);
    CHECK(stats.checkedTriangles == 8 && stats.mixedTriangles == 8 && stats.crossedTriangles == 8);
    CutPick::PickJoints(prism, { kInvalidIndex, 0 }, HorizontalPlane(1, 1, 0, 0.3f), 0.05f, 0.0f, counted, &stats);
    CHECK(stats.checkedTriangles == 8 && stats.mixedTriangles == 8 && stats.crossedTriangles == 2);
    CutPick::PickJoints(prism, { kInvalidIndex, 0 }, HorizontalPlane(0, 3, 0, 5.0f), 0.05f, 0.0f, counted, &stats);
    CHECK(stats.checkedTriangles == 8 && stats.mixedTriangles == 0 && stats.crossedTriangles == 0);

    // 법선이 반대여도 같은 삼각형이다.
    CutPlane flipped = HorizontalPlane(0, 1, 0, 5.0f);
    flipped.RotateAroundLocalAxis(0, XM_PI);
    const std::vector<CutPickJoint> flippedPicked = Pick(prism, { kInvalidIndex, 0 }, flipped);
    CHECK(flippedPicked.size() == 1 && flippedPicked[0].triangleCount == 8);

    // 기둥을 비껴가는 평면은 교차 삼각형이 없다.
    CHECK(Pick(prism, { kInvalidIndex, 0 }, HorizontalPlane(0, 3, 0, 5.0f)).empty());
    CHECK(Pick(prism, { kInvalidIndex, 0 }, HorizontalPlane(0, -1, 0, 5.0f)).empty());
}

static void TestPickJointsRootSide()
{
    // 아래 고리는 joint 1, 위 고리는 joint 2 (2의 부모가 1). 모든 옆면 삼각형이 두 joint를 섞으므로 루트 쪽 joint 1을 고른다.
    const std::vector<CutPickJoint> mixed = Pick(MakePrismMesh(1, 2), { kInvalidIndex, 0, 1 }, HorizontalPlane(0, 1, 0, 5.0f));
    CHECK(mixed.size() == 1 && mixed[0].joint == 1 && mixed[0].triangleCount == 8);

    // 부모와 자식이 바뀌어도 (아래가 자식) 루트에 가까운 joint를 고른다.
    const std::vector<CutPickJoint> swapped = Pick(MakePrismMesh(2, 1), { kInvalidIndex, 0, 1 }, HorizontalPlane(0, 1, 0, 5.0f));
    CHECK(swapped.size() == 1 && swapped[0].joint == 1 && swapped[0].triangleCount == 8);

    // 깊이는 부모를 따라 센다: 0(루트) - 3 - 1, 번호가 아니라 계층이 기준이다.
    const std::vector<CutPickJoint> chain = Pick(MakePrismMesh(1, 3), { kInvalidIndex, 3, 0, 0 }, HorizontalPlane(0, 1, 0, 5.0f));
    CHECK(chain.size() == 1 && chain[0].joint == 3);
}

static void TestPickJointsSiblingTie()
{
    // 형제 joint 1, 2 (같은 깊이). 삼각형 A: 정점 셋의 가중치 합이 joint 2가 크다. 삼각형 B: 합이 같아 번호가 작은 joint 1.
    CutPickMesh mesh;
    mesh.positions = { { 0, -1, 0 }, { 1, 1, 0 }, { 0, 1, 1 }, { 5, -1, 0 }, { 6, 1, 0 }, { 5, 1, 1 } };
    SkinVertex half;
    half.jointCount = 2;
    half.joints[0] = 1;
    half.joints[1] = 2;
    half.weights[0] = 0.5f;
    half.weights[1] = 0.5f;
    mesh.skins = { MakeRigidSkin(1), MakeRigidSkin(2), MakeRigidSkin(2), MakeRigidSkin(1), MakeRigidSkin(2), half };
    mesh.indices = { 0, 1, 2, 3, 4, 5 };
    const std::vector<CutPickJoint> picked = Pick(mesh, { kInvalidIndex, 0, 0 }, HorizontalPlane(3, 0, 0, 10.0f));
    CHECK(picked.size() == 2 && picked[0].joint == 1 && picked[0].triangleCount == 1 && picked[1].joint == 2 && picked[1].triangleCount == 1);
}

static void TestPickJointsTouchAndRectangle()
{
    const CutPickMesh prism = MakePrismMesh(1, 1);

    // 정점이나 에지만 평면에 닿으면 교차가 아니다 (평면 y = 0은 아래 고리 에지 네 개를 지난다). 평면 법선의 float 오차를 epsilon이 흡수한다.
    CHECK(Pick(prism, { kInvalidIndex, 0 }, HorizontalPlane(0, 0, 0, 5.0f), 0.05f, 1e-4f).empty());
    CHECK(Pick(prism, { kInvalidIndex, 0 }, HorizontalPlane(0, 2, 0, 5.0f), 0.05f, 1e-4f).empty());

    // epsilon 안의 정점은 평면 위로 본다: 위 고리(y = 2)에서 0.001 아래의 평면은 epsilon 0.01이면 닿기만 한 것이다.
    CHECK(Pick(prism, { kInvalidIndex, 0 }, HorizontalPlane(0, 1.999f, 0, 5.0f), 0.05f, 0.0f).size() == 1);
    CHECK(Pick(prism, { kInvalidIndex, 0 }, HorizontalPlane(0, 1.999f, 0, 5.0f), 0.05f, 0.01f).empty());

    // 사각형 밖: 절단선이 사각형에 닿지 않으면 제외된다.
    CHECK(Pick(prism, { kInvalidIndex, 0 }, HorizontalPlane(5, 1, 0, 0.5f)).empty());

    // 사각형이 +x 면의 절단선 일부만 덮으면 그 면의 삼각형 둘만 교차 삼각형이다.
    const std::vector<CutPickJoint> partial = Pick(prism, { kInvalidIndex, 0 }, HorizontalPlane(1, 1, 0, 0.3f));
    CHECK(partial.size() == 1 && partial[0].triangleCount == 2);

    // 절단선 선분이 사각형을 가로지르면 양 끝점이 사각형 밖이어도 교차 삼각형이다: 선분 (-1,0,0)-(1,0,0)과 반 크기 0.5의 사각형.
    CutPickMesh wide;
    wide.positions = { { -2.0f, -1.0f, 0.0f }, { 2.0f, -1.0f, 0.0f }, { 0.0f, 1.0f, 0.0f } };
    wide.skins = { MakeRigidSkin(1), MakeRigidSkin(1), MakeRigidSkin(1) };
    wide.indices = { 0, 1, 2 };
    const std::vector<CutPickJoint> crossing = Pick(wide, { kInvalidIndex, 0 }, HorizontalPlane(0, 0, 0, 0.5f), 0.05f, 1e-4f);
    CHECK(crossing.size() == 1 && crossing[0].triangleCount == 1);
    CHECK(Pick(wide, { kInvalidIndex, 0 }, HorizontalPlane(0, 0, 3, 0.5f), 0.05f, 1e-4f).empty());        // 같은 평면 위지만 선분에서 멀다
    CHECK(Pick(wide, { kInvalidIndex, 0 }, HorizontalPlane(0, 0, 0.7f, 0.5f), 0.05f, 1e-4f).empty());     // 사각형이 선분 옆을 지난다
    CHECK(Pick(wide, { kInvalidIndex, 0 }, HorizontalPlane(1.4f, 0, 0, 0.5f), 0.05f, 1e-4f).size() == 1);  // 한 끝점 근처만 덮어도 만난다
}

static void TestPickJointsWeights()
{
    // minWeight 이하의 가중치만 가진 joint는 후보가 아니다: 모든 정점에 루트 joint 0이 0.03씩 섞여 있어도 joint 1을 고른다.
    CutPickMesh prism = MakePrismMesh(1, 1);
    for (SkinVertex& skin : prism.skins)
    {
        skin.jointCount = 2;
        skin.joints[0] = 0;
        skin.joints[1] = 1;
        skin.weights[0] = 0.03f;
        skin.weights[1] = 0.97f;
    }
    const std::vector<uint32_t> parents = { kInvalidIndex, 0 };
    const std::vector<CutPickJoint> filtered = Pick(prism, parents, HorizontalPlane(0, 1, 0, 5.0f), 0.05f);
    CHECK(filtered.size() == 1 && filtered[0].joint == 1);
    const std::vector<CutPickJoint> unfiltered = Pick(prism, parents, HorizontalPlane(0, 1, 0, 5.0f), 0.0f);
    CHECK(unfiltered.size() == 1 && unfiltered[0].joint == 0);

    // 같은 joint가 여러 칸에 있으면 합쳐서 본다: 루트 joint 0의 0.03 두 칸은 합 0.06이라 후보다.
    for (SkinVertex& skin : prism.skins)
    {
        skin.jointCount = 3;
        skin.joints[0] = 0;
        skin.joints[1] = 0;
        skin.joints[2] = 1;
        skin.weights[0] = 0.03f;
        skin.weights[1] = 0.03f;
        skin.weights[2] = 0.94f;
    }
    const std::vector<CutPickJoint> merged = Pick(prism, parents, HorizontalPlane(0, 1, 0, 5.0f), 0.05f);
    CHECK(merged.size() == 1 && merged[0].joint == 0);
}

static void TestPickJointsBadInput()
{
    const CutPickMesh prism = MakePrismMesh(1, 1);
    const std::vector<uint32_t> parents = { kInvalidIndex, 0 };
    std::vector<CutPickJoint> out;
    out.push_back(CutPickJoint());

    // 평면이 올바르지 않으면 비어 있다 (먼저 비워진다).
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CutPick::PickJoints(prism, parents, CutPlane({ nan, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }), 0.05f, 0.0f, out);
    CHECK(out.empty());

    // 정점과 스킨의 크기가 다르면 비어 있다.
    CutPickMesh sizes = prism;
    sizes.skins.pop_back();
    CHECK(Pick(sizes, parents, HorizontalPlane(0, 1, 0, 5.0f)).empty());

    // 범위 밖 인덱스와 3의 배수가 아닌 끝은 건너뛴다.
    CutPickMesh broken = prism;
    broken.indices[2] = 99;
    broken.indices.push_back(0);
    const std::vector<CutPickJoint> picked = Pick(broken, parents, HorizontalPlane(0, 1, 0, 5.0f));
    CHECK(picked.size() == 1 && picked[0].triangleCount == 7);

    // 팔레트 밖 joint나 부모가 순환하는 joint는 후보가 아니다. 고를 joint가 없는 삼각형은 센 적이 없다.
    CHECK(Pick(MakePrismMesh(9, 9), parents, HorizontalPlane(0, 1, 0, 5.0f)).empty());
    CHECK(Pick(MakePrismMesh(1, 1), { 1, 0 }, HorizontalPlane(0, 1, 0, 5.0f)).empty());

    // 스킨이 없는 정점만 있는 삼각형도 건너뛴다.
    CutPickMesh unskinned = prism;
    for (SkinVertex& skin : unskinned.skins)
        skin = SkinVertex();
    CHECK(Pick(unskinned, parents, HorizontalPlane(0, 1, 0, 5.0f)).empty());

    // 비어 있는 메시.
    CHECK(Pick(CutPickMesh(), parents, HorizontalPlane(0, 1, 0, 5.0f)).empty());
}

// 스킨 없는 메시용 교차 판정 (CountCrossedTriangles, SphereMayCross): 시험할 평면 모음. 가장자리에 걸치지 않는 값만 쓴다.
static std::vector<CutPlane> MakeStaticTestPlanes()
{
    std::vector<CutPlane> planes;
    planes.push_back(HorizontalPlane(0, 1, 0, 5.0f));
    planes.push_back(HorizontalPlane(1, 1, 0, 0.3f));
    planes.push_back(HorizontalPlane(5, 1, 0, 0.5f));
    planes.push_back(HorizontalPlane(0, 3, 0, 5.0f));
    planes.push_back(HorizontalPlane(0, 0.5f, 0, 5.0f));
    for (int i = 0; i < 8; ++i)
    {
        XMFLOAT4 rotation;
        XMStoreFloat4(&rotation, XMQuaternionRotationRollPitchYaw(0.3f + 0.4f * i, 0.5f * i - 0.7f, 0.9f - 0.2f * i));
        planes.push_back(CutPlane({ 0.1f * i - 0.3f, 0.6f + 0.15f * i, 0.2f - 0.05f * i }, rotation, { 0.4f + 0.3f * i, 1.5f - 0.1f * i }));
    }
    return planes;
}

static XMFLOAT4X4 MakeStaticTestWorld(int variant)
{
    XMMATRIX m = XMMatrixIdentity();
    if (variant == 1)
        m = XMMatrixTranslation(0.5f, -0.2f, 1.0f);
    else if (variant == 2)
        m = XMMatrixRotationRollPitchYaw(0.3f, 0.7f, -0.4f) * XMMatrixTranslation(-0.5f, 0.4f, 0.2f);
    else if (variant == 3)
        m = XMMatrixScaling(1.0f, 2.0f, 0.5f) * XMMatrixRotationRollPitchYaw(0.3f, 0.7f, 0.1f) * XMMatrixTranslation(0.5f, -0.2f, 1.0f);
    XMFLOAT4X4 world;
    XMStoreFloat4x4(&world, m);
    return world;
}

// 로컬 기둥 정점을 world로 옮긴 CutPickMesh
static CutPickMesh MakeWorldPrism(const CutPickMesh& local, const XMFLOAT4X4& world)
{
    CutPickMesh mesh = local;
    for (XMFLOAT3& p : mesh.positions)
        XMStoreFloat3(&p, XMVector3TransformCoord(XMLoadFloat3(&p), XMLoadFloat4x4(&world)));
    return mesh;
}

static uint32_t TotalPicked(const std::vector<CutPickJoint>& picked)
{
    uint32_t total = 0;
    for (const CutPickJoint& joint : picked)
        total += joint.triangleCount;
    return total;
}

static void TestCountCrossedMatchesPickJoints()
{
    // 같은 입력(단위 행렬)에서 교차 수와 계측 개수가 PickJoints와 같다: 여러 평면, epsilon 0과 1e-3
    const CutPickMesh prism = MakePrismMesh(1, 1);
    const XMFLOAT4X4 identity = MakeStaticTestWorld(0);
    for (const CutPlane& plane : MakeStaticTestPlanes())
        for (float epsilon : { 0.0f, 1e-3f })
        {
            std::vector<CutPickJoint> picked;
            CutPickStats pickStats, countStats;
            CutPick::PickJoints(prism, { kInvalidIndex, 0 }, plane, 0.05f, epsilon, picked, &pickStats);
            const uint32_t crossed = CutPick::CountCrossedTriangles(prism.positions, prism.indices, identity, plane, epsilon, &countStats);
            CHECK(crossed == TotalPicked(picked) && crossed == pickStats.crossedTriangles);
            CHECK(countStats.checkedTriangles == pickStats.checkedTriangles && countStats.mixedTriangles == pickStats.mixedTriangles
                && countStats.crossedTriangles == pickStats.crossedTriangles);
        }

    // 정점이나 에지만 닿는 평면, epsilon 안의 평면은 PickJoints처럼 교차가 아니다. 올바르지 않은 평면은 0.
    CHECK(CutPick::CountCrossedTriangles(prism.positions, prism.indices, identity, HorizontalPlane(0, 0, 0, 5.0f), 1e-4f) == 0);
    CHECK(CutPick::CountCrossedTriangles(prism.positions, prism.indices, identity, HorizontalPlane(0, 1.999f, 0, 5.0f), 0.01f) == 0);
    CHECK(CutPick::CountCrossedTriangles(prism.positions, prism.indices, identity, HorizontalPlane(0, 1.999f, 0, 5.0f), 0.0f) == 8);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    CHECK(CutPick::CountCrossedTriangles(prism.positions, prism.indices, identity,
        CutPlane({ nan, 1.0f, 0.0f }, { 0.0f, 0.0f, 0.0f, 1.0f }, { 1.0f, 1.0f }), 0.0f) == 0);
}

static void TestCountCrossedLocalPlane()
{
    // 평면을 로컬로 옮긴 판정(정점은 그대로)이 정점을 world로 옮긴 PickJoints와 같다: 이동, 회전, 비균등 크기
    const CutPickMesh prism = MakePrismMesh(1, 1);
    for (int variant = 0; variant < 4; ++variant)
    {
        const XMFLOAT4X4 world = MakeStaticTestWorld(variant);
        const CutPickMesh worldPrism = MakeWorldPrism(prism, world);
        for (const CutPlane& plane : MakeStaticTestPlanes())
        {
            std::vector<CutPickJoint> picked;
            CutPickStats pickStats, countStats;
            CutPick::PickJoints(worldPrism, { kInvalidIndex, 0 }, plane, 0.05f, 1e-4f, picked, &pickStats);
            const uint32_t crossed = CutPick::CountCrossedTriangles(prism.positions, prism.indices, world, plane, 1e-4f, &countStats);
            CHECK(crossed == TotalPicked(picked));
            CHECK(countStats.mixedTriangles == pickStats.mixedTriangles);
        }
    }
}

static void TestSphereMayCross()
{
    // 보수적 판정: 실제로 교차 삼각형이 있으면 항상 true (여러 행렬, 평면). 기둥의 바운딩 구는 중심 (0, 1, 0), 반지름 sqrt(3).
    const CutPickMesh prism = MakePrismMesh(1, 1);
    const XMFLOAT4 sphere = { 0.0f, 1.0f, 0.0f, std::sqrt(3.0f) };
    for (int variant = 0; variant < 4; ++variant)
    {
        const XMFLOAT4X4 world = MakeStaticTestWorld(variant);
        for (const CutPlane& plane : MakeStaticTestPlanes())
            if (CutPick::CountCrossedTriangles(prism.positions, prism.indices, world, plane, 0.0f) > 0)
                CHECK(CutPick::SphereMayCross(sphere, world, plane, 0.0f));
    }

    // 경계: 구 위쪽 끝(y = 1 + sqrt(3))을 조금 지난 평면은 false, 조금 못 미친 평면은 true. 사각형 쪽 경계도 같다.
    const XMFLOAT4X4 identity = MakeStaticTestWorld(0);
    const float top = 1.0f + std::sqrt(3.0f);
    CHECK(!CutPick::SphereMayCross(sphere, identity, HorizontalPlane(0, top + 0.01f, 0, 5.0f), 0.0f));
    CHECK(CutPick::SphereMayCross(sphere, identity, HorizontalPlane(0, top - 0.01f, 0, 5.0f), 0.0f));
    CHECK(CutPick::SphereMayCross(sphere, identity, HorizontalPlane(0, top + 0.01f, 0, 5.0f), 0.05f));       // epsilon만큼 넓힌다
    const float side = 0.5f + std::sqrt(3.0f);      // 반 크기 0.5인 사각형이 구와 만나는 x 거리
    CHECK(!CutPick::SphereMayCross(sphere, identity, HorizontalPlane(side + 0.01f, 1, 0, 0.5f), 0.0f));
    CHECK(CutPick::SphereMayCross(sphere, identity, HorizontalPlane(side - 0.01f, 1, 0, 0.5f), 0.0f));

    // 크기가 반지름에 반영된다: 2배 확대하면 같은 평면(y = top + 0.5)이 이제 구와 만난다 (구 중심도 2배 위로 간다).
    XMFLOAT4X4 scaled;
    XMStoreFloat4x4(&scaled, XMMatrixScaling(2.0f, 2.0f, 2.0f));
    CHECK(CutPick::SphereMayCross(sphere, scaled, HorizontalPlane(0, 2.0f * top - 0.01f, 0, 5.0f), 0.0f));
    CHECK(!CutPick::SphereMayCross(sphere, scaled, HorizontalPlane(0, 2.0f * top + 0.01f, 0, 5.0f), 0.0f));

    // 믿을 수 없는 구(반지름 0)는 건너뛰지 않는다.
    CHECK(CutPick::SphereMayCross({ 0.0f, 0.0f, 0.0f, 0.0f }, identity, HorizontalPlane(0, 100.0f, 0, 1.0f), 0.0f));
}


// ---------------------------------------------------------------- SliceProceduralMesh (U-2)

// 두 조각의 노드, 코너, 면, 경계, cap 기록이 같은가
static bool SamePiece(const MeshGeometry& a, const MeshGeometry& b)
{
    if (a.vertices.size() != b.vertices.size() || a.corners.size() != b.corners.size() || a.faces.size() != b.faces.size()
        || a.edgeTags != b.edgeTags || a.caps.size() != b.caps.size() || a.skin != b.skin)
        return false;
    for (size_t i = 0; i < a.vertices.size(); ++i)
        if (!SamePosition(a.vertices[i].position, b.vertices[i].position) || a.vertices[i].kind != b.vertices[i].kind || a.vertices[i].cutId != b.vertices[i].cutId)
            return false;
    for (size_t i = 0; i < a.corners.size(); ++i)
        if (a.corners[i].vertex != b.corners[i].vertex || !SamePosition(a.corners[i].normal, b.corners[i].normal))
            return false;
    for (size_t i = 0; i < a.faces.size(); ++i)
    {
        const MeshFace &fa = a.faces[i], &fb = b.faces[i];
        if (fa.kind != fb.kind || fa.cutId != fb.cutId || fa.sourceTriangle != fb.sourceTriangle
            || fa.corner[0] != fb.corner[0] || fa.corner[1] != fb.corner[1] || fa.corner[2] != fb.corner[2])
            return false;
    }
    for (size_t i = 0; i < a.caps.size(); ++i)
        if (a.caps[i].cutId != b.caps[i].cutId || a.caps[i].rimVertices != b.caps[i].rimVertices || a.caps[i].loopSizes != b.caps[i].loopSizes
            || a.caps[i].materialSlot != b.caps[i].materialSlot || a.caps[i].inLastSection != b.caps[i].inLastSection)
            return false;
    return true;
}

// 같은 입력에서 SliceGeometry(cutId 1, cap 생성)와 같은 조각을 낸다. 실패하면 입력이 그대로다.
static void TestSliceProceduralMeshMatchesSliceGeometry()
{
    const MeshGeometry box = MakeBox();
    const SlicePlane plane = MakePlane(0, 0.5f, 0, 0.3f, 1, 0);
    GeometrySliceResult reference;
    CHECK_STATUS(SliceGeometry(box, plane, 1, CapOptions(), reference), SliceStatus::Ok, reference.detail);

    MeshGeometry mesh = box, other;
    SliceStatus status = SliceStatus::InvalidInput;
    std::string error = "stale";
    std::vector<std::vector<uint32_t>> loops;
    CHECK(SliceTestGeometry(mesh, plane.point, plane.normal, true, &other, SliceCapOption::CreateNewSectionForCap, 1,
        &status, &error, &loops));
    CHECK(status == SliceStatus::Ok && error.empty());
    CHECK(SamePiece(mesh, reference.positive) && SamePiece(other, reference.negative));
    CHECK(loops == reference.loops);

    // 기본 동작(새 section, 슬롯 1)의 렌더 출력은 SliceGeometry 결과와 바이트 단위로 같다
    ProceduralMeshData mine, theirs;
    BuildRenderMesh(mesh, mine);
    BuildRenderMesh(reference.positive, theirs);
    CHECK(mine.indices == theirs.indices && mine.vertices.size() == theirs.vertices.size()
        && std::memcmp(mine.vertices.data(), theirs.vertices.data(), mine.vertices.size() * sizeof(MeshRenderVertex)) == 0);
    CHECK(mine.sections.size() == theirs.sections.size());
    for (size_t i = 0; i < mine.sections.size() && i < theirs.sections.size(); ++i)
        CHECK(mine.sections[i].firstIndex == theirs.sections[i].firstIndex && mine.sections[i].indexCount == theirs.sections[i].indexCount
            && mine.sections[i].materialSlot == theirs.sections[i].materialSlot);

    // 두 번째 절단은 다음 cutId(2)를 쓴다
    MeshGeometry again = mesh;
    GeometrySliceResult second;
    CHECK_STATUS(SliceGeometry(mesh, MakePlane(0, 1.5f, 0, 0, 1, 0.2f), 2, CapOptions(), second), SliceStatus::Ok, second.detail);
    CHECK(SliceTestGeometry(again, { 0, 1.5f, 0 }, { 0, 1, 0.2f }, false, nullptr, SliceCapOption::CreateNewSectionForCap, 1));
    CHECK(SamePiece(again, second.positive));

    // 실패(빗나감): 입력, 다른 쪽, 루프가 그대로다
    MeshGeometry missed = box, untouched = reference.negative;
    std::vector<std::vector<uint32_t>> keptLoops = reference.loops;
    CHECK(!SliceTestGeometry(missed, { 0, 5, 0 }, { 0, 1, 0 }, true, &untouched, SliceCapOption::CreateNewSectionForCap, 1,
        &status, &error, &keptLoops));
    CHECK(status == SliceStatus::NoCut && !error.empty());
    CHECK(SamePiece(missed, box) && SamePiece(untouched, reference.negative) && keptLoops == reference.loops);
    // 실패(잘못된 법선)
    CHECK(!SliceTestGeometry(missed, { 0, 1, 0 }, { 0, 0, 0 }, true, &untouched, SliceCapOption::NoCap, 1, &status));
    CHECK(status == SliceStatus::InvalidInput && SamePiece(missed, box) && SamePiece(untouched, reference.negative));

    // 법선 길이는 결과를 바꾸지 않는다 (SliceGeometry와 같은 규칙)
    MeshGeometry scaled = box;
    CHECK(SliceTestGeometry(scaled, plane.point, { 3.0f, 10.0f, 0 }, false, nullptr, SliceCapOption::CreateNewSectionForCap, 1));
    CHECK(scaled.faces.size() == reference.positive.faces.size() && scaled.vertices.size() == reference.positive.vertices.size());

    // createOtherHalf = false: 다른 쪽은 빈 조각이다
    MeshGeometry half = box, empty = reference.negative;
    CHECK(SliceTestGeometry(half, plane.point, plane.normal, false, &empty, SliceCapOption::CreateNewSectionForCap, 1));
    CHECK(SamePiece(half, reference.positive) && empty.Empty() && empty.vertices.empty() && empty.caps.empty());
}

// 세 cap option의 section 구성
static void TestSliceCapOptions()
{
    const MeshGeometry box = MakeBox();
    const XMFLOAT3 point = { 0, 1, 0 }, normal = { 0, 1, 0 };

    // NoCap: cap 면과 cap 기록이 없고, 표면 section 하나뿐이다
    MeshGeometry open = box, openOther;
    CHECK(SliceTestGeometry(open, point, normal, true, &openOther, SliceCapOption::NoCap, 3));
    CHECK(CountCapFaces(open) == 0 && open.caps.empty() && CountCapFaces(openOther) == 0);
    ProceduralMeshData baked;
    BuildRenderMesh(open, baked);
    CHECK(baked.sections.size() == 1 && baked.sections[0].materialSlot == 0);

    // CreateNewSectionForCap: [표면 0][cap capMaterialSlot], 양쪽 조각 모두
    MeshGeometry created = box, createdOther;
    CHECK(SliceTestGeometry(created, point, normal, true, &createdOther, SliceCapOption::CreateNewSectionForCap, 3));
    for (const MeshGeometry* piece : { &created, &createdOther })
    {
        CHECK(piece->caps.size() == 1 && piece->caps[0].materialSlot == 3 && !piece->caps[0].inLastSection);
        BuildRenderMesh(*piece, baked);
        CHECK(baked.sections.size() == 2 && baked.sections[0].materialSlot == 0 && baked.sections[1].materialSlot == 3);
        CHECK(CountSectionFaces(baked, 1) == CountCapFaces(*piece));
    }

    // UseLastSectionForCap: cap 면이 표면 section 하나에 합쳐진다 (표면 면 뒤에)
    MeshGeometry merged = box, mergedOther;
    CHECK(SliceTestGeometry(merged, point, normal, true, &mergedOther, SliceCapOption::UseLastSectionForCap, 3));
    for (const MeshGeometry* piece : { &merged, &mergedOther })
    {
        CHECK(piece->caps.size() == 1 && piece->caps[0].inLastSection);
        BuildRenderMesh(*piece, baked);
        CHECK(baked.sections.size() == 1 && baked.sections[0].materialSlot == 0);
        CHECK(CountSectionFaces(baked, 0) == piece->faces.size());
        const size_t surfaceIndices = (piece->faces.size() - CountCapFaces(*piece)) * 3;
        const MeshFace& lastFace = piece->faces.back();
        CHECK(lastFace.kind == MeshFaceKind::Cap && baked.indices[baked.indices.size() - 1] == lastFace.corner[2]);
        CHECK(baked.indices.size() > surfaceIndices);
    }
    // splitByOrigin이면 마지막 표면 section은 "나뉜 면" section이다
    RenderBakeOptions split;
    split.splitByOrigin = true;
    BuildRenderMesh(merged, baked, split);
    CHECK(baked.sections.size() == 2 && baked.sections[1].materialSlot == 0);
    uint32_t divided = 0;
    for (const MeshFace& face : merged.faces)
        divided += face.kind == MeshFaceKind::Surface && face.cutId != 0;
    CHECK(CountSectionFaces(baked, 1) == divided + CountCapFaces(merged));

    // 섞인 두 절단: cut 1은 슬롯 1의 새 section, cut 2는 마지막 표면 section
    MeshGeometry mixed = box;
    CHECK(SliceTestGeometry(mixed, { 0, 0.5f, 0 }, { 0.3f, 1, 0 }, false, nullptr, SliceCapOption::CreateNewSectionForCap, 1));
    CHECK(SliceTestGeometry(mixed, { 0, 1.5f, 0 }, { 0, -1, 0.2f }, false, nullptr, SliceCapOption::UseLastSectionForCap, 2));
    CHECK(mixed.caps.size() == 2);
    BuildRenderMesh(mixed, baked);
    uint32_t firstCap = 0, secondCap = 0;
    for (const MeshFace& face : mixed.faces)
        if (face.kind == MeshFaceKind::Cap)
            (face.cutId == 1 ? firstCap : secondCap) += 1;
    CHECK(firstCap > 0 && secondCap > 0);
    CHECK(baked.sections.size() == 2 && baked.sections[1].materialSlot == 1 && CountSectionFaces(baked, 1) == firstCap);
    CHECK(CountSectionFaces(baked, 0) == mixed.faces.size() - firstCap);
}

int main()
{
    TestWeldBox();
    TestWeldUvSeamTube();
    TestWeldSkinSplitSeam();
    TestWeldNegativeZero();
    TestInputRejects();
    TestValidatePlane();
    TestUntaggedBoundaryIsRejected();
    TestOpenGridBoundaryKinds();
    TestBoundaryDebugBake();

    TestBoxCenterCut();
    TestPlaneThroughNodes();
    TestOneNodeOnPlane();
    TestOpenSheetIsOpenLoop();
    TestOpenSurfaceChainHasNoCap();
    TestTouchingIsNoCut();
    TestCoplanarFace();
    TestMissIsNoCut();
    TestFailureClearsOut();

    TestBakeSharesNodePositions();
    TestOriginLimit();
    TestCapFaceIsNotCrossed();
    TestTiltedCutKeepsArea();
    TestSliverOutputIsRejected();

    TestUvSeamTubeCut();
    TestSkinSplitSeamIsOpenLoop();
    TestThickTubeHoleLoops();
    TestSharedNodeHasNoCap();
    TestInvalidLoopsHaveNoCap();

    TestCapIsWatertight();
    TestCapWithHole();
    TestCapOfConcaveLoop();
    TestRecut();
    TestSplitFacesRecordCutId();
    TestSplitByOriginBake();
    TestSliceProceduralMeshMatchesSliceGeometry();
    TestSliceCapOptions();
    TestLoopDebugBake();
    TestCutPlaneFrame();
    TestCutPlaneAccumulatedRotation();
    TestCutPlaneContains();
    TestCutPlaneScale();
    TestCutPlaneFactories();
    TestCutPlaneToSlicePlane();
    TestLoopsInsidePlane();
    TestPickChain();
    TestPickThroughJoint();
    TestPickSeveralSegments();
    TestPickSharedParent();
    TestPickRejectsBadInput();
    TestJointSet();
    TestGizmoRay();
    TestGizmoAxisDrag();
    TestGizmoRotateDrag();
    TestGizmoScaleDrag();
    TestGizmoPicking();
    TestGizmoStateMachine();
    TestGizmoBake();

    TestSkinIdentity();
    TestSkinSingleJoint();
    TestSkinTwoJointBlend();
    TestSkinMatchesSliceNodes();
    TestSkinKeepsSeamsAndCapsTogether();
    TestSkinRefusesSkinState();
    TestSkinAfterTwoCuts();
    TestSkinningShared();
    TestSkinRefusesBadInput();
    TestRigidWeldAndSkin();
    TestRigidSlice();
    TestRigidOpenBoxSlice();
    TestProceduralMeshOwnsGeometry();
    TestPublicProceduralMeshPipeline();

    TestCutJobKey();
    TestDiffJobs();
    TestJobColors();

    TestPickJointsSingleJoint();
    TestPickJointsRootSide();
    TestPickJointsSiblingTie();
    TestPickJointsTouchAndRectangle();
    TestPickJointsWeights();
    TestPickJointsBadInput();
    TestCountCrossedMatchesPickJoints();
    TestCountCrossedLocalPlane();
    TestSphereMayCross();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures;
}
