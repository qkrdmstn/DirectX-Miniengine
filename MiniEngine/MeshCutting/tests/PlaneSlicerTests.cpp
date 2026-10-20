// Standalone tests for the slicing pipeline (Geometry/). No D3D12 device is needed.
//
// Build and run from a Visual Studio developer prompt in MiniEngine/MeshCutting:
//   cl /nologo /EHsc /std:c++14 /W4 /MDd /Fo<outdir>\ /Fe<outdir>\PlaneSlicerTests.exe
//      tests\PlaneSlicerTests.cpp Geometry\*.cpp ProceduralMesh.cpp
//   <outdir>\PlaneSlicerTests.exe
// Keep <outdir> outside the repository. The exit code is the number of failed checks.

#include "../Geometry/PatchGeometry.h"
#include "../Geometry/PatchWeld.h"
#include "../Geometry/PieceSkinning.h"
#include "../Geometry/PlaneSlicer.h"
#include "../Geometry/RenderBake.h"
#include "../ProceduralMesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <vector>

using namespace MeshCutting;
using DirectX::XMFLOAT3;

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

// One node and one render corner per position, identity skin.
static PatchPiece MakePiece(const std::vector<XMFLOAT3>& positions, const std::vector<Tri>& tris)
{
    PatchPiece piece;
    for (size_t i = 0; i < positions.size(); ++i)
    {
        SkinNode skin;
        skin.bindPosition = positions[i];
        skin.jointCount = 1;
        skin.joints[0] = 0;
        skin.weights[0] = 1.0f;
        piece.originNodes.push_back(skin);

        GeomNode node;
        node.origin.node[0] = uint32_t(i);
        node.origin.bary[0] = 1.0f;
        node.position = positions[i];
        piece.nodes.push_back(node);

        RenderCorner corner;
        corner.node = uint32_t(i);
        corner.uv = { positions[i].x, positions[i].z };
        piece.corners.push_back(corner);
    }
    for (size_t i = 0; i < tris.size(); ++i)
    {
        Face face;
        for (int k = 0; k < 3; ++k)
            face.corner[k] = tris[i][k];
        face.sourceTriangle = uint32_t(i);
        piece.faces.push_back(face);
    }
    return piece;
}

// Tags every boundary edge of a hand-built piece the way the weld step does for open patches.
static void TagOpenEdges(PatchPiece& piece, EdgeKind kind)
{
    EdgeUsageMap usage;
    CollectEdgeUsage(piece, usage);
    for (const auto& entry : usage)
        if (entry.second.count == 1)
            piece.boundary[entry.first] = kind;
}

static PatchPiece MakeBox()
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
static WeldInput MakeTubeWeldInput(int n, bool seamDuplicate, uint16_t seamJoint)
{
    const double kTwoPi = 6.283185307179586;
    WeldInput in;
    const int columns = seamDuplicate ? n + 1 : n;
    for (int ring = 0; ring < 2; ++ring)
    {
        for (int c = 0; c < columns; ++c)
        {
            const bool dup = seamDuplicate && c == n;
            const double angle = kTwoPi * double(dup ? 0 : c) / double(n);
            WeldInputVertex v;
            v.bindPosition = { float(std::cos(angle)), float(ring * 2), float(std::sin(angle)) };
            v.position = v.bindPosition;
            v.uv = { float(c) / float(n), float(ring) };
            v.joints[0] = dup ? seamJoint : 0;
            v.weights[0] = 1.0f;
            in.vertices.push_back(v);
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

static WeldInput MakeBoxWeldInput()
{
    const PatchPiece box = MakeBox();
    WeldInput in;
    for (size_t i = 0; i < box.nodes.size(); ++i)
    {
        WeldInputVertex v;
        v.bindPosition = box.nodes[i].position;
        v.position = box.nodes[i].position;
        v.joints[0] = 0;
        v.weights[0] = 1.0f;
        in.vertices.push_back(v);
    }
    for (const Face& f : box.faces)
        in.indices.insert(in.indices.end(), { f.corner[0], f.corner[1], f.corner[2] });
    return in;
}

static uint32_t CountKind(const PatchPiece& piece, EdgeKind kind)
{
    uint32_t n = 0;
    for (const auto& entry : piece.boundary)
        if (entry.second == kind)
            ++n;
    return n;
}

// ---------------------------------------------------------------- 2-1 tests

static void TestWeldBox()
{
    PatchPiece piece;
    WeldStats stats;
    std::string detail;
    const SliceStatus status = BuildPatchPiece(MakeBoxWeldInput(), kDefaultEpsilonScale, piece, stats, &detail);
    CHECK_STATUS(status, SliceStatus::Ok, detail);
    CHECK(stats.corners == 8 && stats.nodes == 8 && stats.faces == 12);
    CHECK(stats.boundaryEdges == 0 && piece.boundary.empty());
    CHECK(piece.originNodes.size() == 8 && piece.nodes[3].origin.node[0] == 3 && piece.nodes[3].origin.bary[0] == 1.0f);
    CHECK(piece.faces[5].sourceTriangle == 5);
}

static void TestWeldUvSeamTube()
{
    // Same position and skin on both sides of the UV seam: welded into one node per column.
    PatchPiece piece;
    WeldStats stats;
    std::string detail;
    const SliceStatus status = BuildPatchPiece(MakeTubeWeldInput(8, true, 0), kDefaultEpsilonScale, piece, stats, &detail);
    CHECK_STATUS(status, SliceStatus::Ok, detail);
    CHECK(stats.corners == 18 && stats.nodes == 16);
    CHECK(stats.positionNodes == 16 && stats.skinSplitPositions == 0);
    CHECK(stats.unweldedSeamEdges == 0);
    CHECK(stats.replacementEdges == 16);        // 8 bottom + 8 top rim edges
    CHECK(CountKind(piece, EdgeKind::Replacement) == 16);
    // The two seam corners keep their own UVs but share one node (and so one position).
    CHECK(piece.corners[0].node == piece.corners[8].node);
    CHECK(piece.corners[0].uv.x == 0.0f && piece.corners[8].uv.x == 1.0f);
}

static void TestWeldSkinSplitSeam()
{
    // Seam duplicates with a different joint are not welded; the seam stays open and is tagged.
    PatchPiece piece;
    WeldStats stats;
    std::string detail;
    const SliceStatus status = BuildPatchPiece(MakeTubeWeldInput(8, true, 1), kDefaultEpsilonScale, piece, stats, &detail);
    CHECK_STATUS(status, SliceStatus::Ok, detail);
    CHECK(stats.corners == 18 && stats.nodes == 18);
    CHECK(stats.positionNodes == 16 && stats.skinSplitPositions == 2);
    CHECK(stats.unweldedSeamEdges == 2);        // the vertical edge on each side of the seam
    CHECK(CountKind(piece, EdgeKind::UnweldedSeam) == 2);
    CHECK(stats.replacementEdges == 16);
}

static void TestWeldNegativeZero()
{
    WeldInput in = MakeBoxWeldInput();
    for (WeldInputVertex& v : in.vertices)
        if (v.bindPosition.x == -1.0f && v.bindPosition.y == 0.0f && v.bindPosition.z == -1.0f)
            v.bindPosition.y = -0.0f;
    in.vertices.push_back(in.vertices[0]);           // duplicate of node 0
    in.vertices.back().bindPosition.y = 0.0f;        // +0 against -0
    in.indices.insert(in.indices.end(), { 8, 1, 2 });  // makes the duplicate used (shares edge 1-2 reversed)
    PatchPiece piece;
    WeldStats stats;
    std::string detail;
    BuildPatchPiece(in, kDefaultEpsilonScale, piece, stats, &detail);
    CHECK(stats.corners == 9 && stats.nodes == 8);   // -0 and +0 weld together
}

static void TestInputRejects()
{
    PatchPiece piece;
    WeldStats stats;
    std::string detail;

    WeldInput nanPosition = MakeBoxWeldInput();
    nanPosition.vertices[2].position.x = std::numeric_limits<float>::quiet_NaN();
    CHECK_STATUS(BuildPatchPiece(nanPosition, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::InvalidInput, detail);
    CHECK(piece.Empty());

    WeldInput badIndex = MakeBoxWeldInput();
    badIndex.indices[4] = 99;
    CHECK_STATUS(BuildPatchPiece(badIndex, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::InvalidInput, detail);

    WeldInput noWeight = MakeBoxWeldInput();
    noWeight.vertices[1].weights[0] = 0.0f;
    CHECK_STATUS(BuildPatchPiece(noWeight, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::InvalidInput, detail);

    WeldInput badCount = MakeBoxWeldInput();
    badCount.indices.pop_back();
    CHECK_STATUS(BuildPatchPiece(badCount, kDefaultEpsilonScale, piece, stats, &detail), SliceStatus::InvalidInput, detail);

    // Hand-edited pieces for the structural checks.
    const PatchPiece good = MakeBox();
    CHECK_STATUS(ValidatePatchPiece(good, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);

    PatchPiece badOrigin = good;
    badOrigin.nodes[3].origin.bary[0] = 0.5f;
    CHECK_STATUS(ValidatePatchPiece(badOrigin, kDefaultEpsilonScale, &detail), SliceStatus::InvalidOrigin, detail);

    PatchPiece badOriginNode = good;
    badOriginNode.nodes[3].origin.node[0] = 99;
    CHECK_STATUS(ValidatePatchPiece(badOriginNode, kDefaultEpsilonScale, &detail), SliceStatus::InvalidOrigin, detail);

    PatchPiece unsortedOrigin = good;
    unsortedOrigin.nodes[3].origin.node[0] = 5;
    unsortedOrigin.nodes[3].origin.node[1] = 3;
    unsortedOrigin.nodes[3].origin.bary[0] = 0.5f;
    unsortedOrigin.nodes[3].origin.bary[1] = 0.5f;
    CHECK_STATUS(ValidatePatchPiece(unsortedOrigin, kDefaultEpsilonScale, &detail), SliceStatus::InvalidOrigin, detail);

    PatchPiece badCorner = good;
    badCorner.corners[2].node = 99;
    CHECK_STATUS(ValidatePatchPiece(badCorner, kDefaultEpsilonScale, &detail), SliceStatus::InvalidInput, detail);

    PatchPiece badFace = good;
    badFace.faces[0].corner[1] = 99;
    CHECK_STATUS(ValidatePatchPiece(badFace, kDefaultEpsilonScale, &detail), SliceStatus::InvalidInput, detail);

    PatchPiece repeated = good;
    repeated.faces[0].corner[1] = repeated.faces[0].corner[0];
    CHECK_STATUS(ValidatePatchPiece(repeated, kDefaultEpsilonScale, &detail), SliceStatus::DegenerateInput, detail);

    PatchPiece flat = good;
    flat.nodes[1].position = flat.nodes[0].position;   // collapses faces that use the edge 0-1
    flat.nodes[1].position.x += 1e-9f;
    CHECK_STATUS(ValidatePatchPiece(flat, kDefaultEpsilonScale, &detail), SliceStatus::DegenerateInput, detail);

    // Three faces on one edge.
    PatchPiece fin = good;
    fin.nodes.push_back(fin.nodes[0]);
    fin.nodes.back().position = { 0.0f, 1.0f, -3.0f };
    fin.nodes.back().origin.node[0] = 8;
    fin.originNodes.push_back(fin.originNodes[0]);
    RenderCorner extra;
    extra.node = 8;
    fin.corners.push_back(extra);
    Face fan;
    fan.corner[0] = 0; fan.corner[1] = 1; fan.corner[2] = 8;   // edge 0-1 already has two faces
    fin.faces.push_back(fan);
    CHECK_STATUS(ValidatePatchPiece(fin, kDefaultEpsilonScale, &detail), SliceStatus::NonManifoldInput, detail);

    // Two faces use an interior edge in the same direction (one face flipped).
    PatchPiece flipped = good;
    std::swap(flipped.faces[0].corner[1], flipped.faces[0].corner[2]);
    CHECK_STATUS(ValidatePatchPiece(flipped, kDefaultEpsilonScale, &detail), SliceStatus::NonManifoldInput, detail);

    // A non-finite or negative epsilon scale is rejected as well.
    CHECK_STATUS(ValidatePatchPiece(good, -1.0f, &detail), SliceStatus::InvalidInput, detail);
}

// An open edge without a kind (or tagged Interior/CapSeam) is rejected up front.
static void TestUntaggedBoundaryIsRejected()
{
    PatchPiece sheet = MakePiece({ { 0, 0, 0 }, { 1, 0, 0 }, { 0, 1, 0 } }, { { 0, 1, 2 } });
    std::string detail;
    CHECK_STATUS(ValidatePatchPiece(sheet, kDefaultEpsilonScale, &detail), SliceStatus::InvalidInput, detail);
    TagOpenEdges(sheet, EdgeKind::Replacement);
    CHECK_STATUS(ValidatePatchPiece(sheet, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    sheet.boundary[MakeEdgeKey(0, 1)] = EdgeKind::CapSeam;
    CHECK_STATUS(ValidatePatchPiece(sheet, kDefaultEpsilonScale, &detail), SliceStatus::InvalidInput, detail);
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
    PatchPiece grid = MakePiece(p, t);
    TagOpenEdges(grid, EdgeKind::Replacement);
    std::string detail;
    CHECK_STATUS(ValidatePatchPiece(grid, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    CHECK(CountKind(grid, EdgeKind::Replacement) == 12);   // perimeter of a 3x3 sheet
}

static void TestBoundaryDebugBake()
{
    PatchPiece piece;
    WeldStats stats;
    std::string detail;
    BuildPatchPiece(MakeTubeWeldInput(8, true, 1), kDefaultEpsilonScale, piece, stats, &detail);
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

static uint32_t CountCutNodes(const PatchPiece& piece, uint32_t cutId)
{
    uint32_t n = 0;
    for (const GeomNode& node : piece.nodes)
        if (node.cutId == cutId)
            ++n;
    return n;
}

static double TotalSurfaceArea(const PatchPiece& piece) { return TotalArea(piece, FaceKind::Surface); }

// Both output pieces must pass the same validation as an input piece.
static void CheckPieceValid(const PatchPiece& piece, const char* label)
{
    std::string detail;
    const SliceStatus status = ValidatePatchPiece(piece, kDefaultEpsilonScale, &detail);
    ++g_checks;
    if (status != SliceStatus::Ok)
    {
        ++g_failures;
        std::printf("FAIL %s: output piece is invalid: %s (%s)\n", label, SliceStatusName(status), detail.c_str());
    }
}

static PatchPiece MakeOctahedron()
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
// The plane crosses 4 vertical edges and the 4 diagonals (0-5, 1-6, 2-7, 3-4) = 8 cut nodes.
static void TestBoxCenterCut()
{
    const PatchPiece box = MakeBox();
    SliceResult result;
    const SliceStatus status = SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result);
    CHECK_STATUS(status, SliceStatus::Ok, result.detail);
    CHECK(result.positive.nodes.size() == 12 && result.negative.nodes.size() == 12);   // 4 box nodes + 8 cut nodes
    CHECK(CountCutNodes(result.positive, 1) == 8 && CountCutNodes(result.negative, 1) == 8);
    CHECK(CountKind(result.positive, EdgeKind::Cut) == 8 && CountKind(result.negative, EdgeKind::Cut) == 8);
    CHECK(result.positive.faces.size() + result.negative.faces.size() > box.faces.size());
    CheckPieceValid(result.positive, "box +");
    CheckPieceValid(result.negative, "box -");

    // The two pieces cover exactly the original surface.
    const double original = TotalSurfaceArea(box);
    const double sum = TotalSurfaceArea(result.positive) + TotalSurfaceArea(result.negative);
    CHECK(std::fabs(sum - original) < original * 1e-5);

    // Cut nodes lie on the plane and are shared by both pieces (same position bits).
    for (const GeomNode& node : result.positive.nodes)
        if (node.cutId == 1)
            CHECK(std::fabs(node.position.y - 1.0f) < 1e-6f);
    for (const GeomNode& a : result.positive.nodes)
    {
        if (a.cutId != 1)
            continue;
        bool matched = false;
        for (const GeomNode& b : result.negative.nodes)
            matched = matched || (b.cutId == 1 && b.position.x == a.position.x && b.position.y == a.position.y
                && b.position.z == a.position.z);
        CHECK(matched);
    }
    // Positive side keeps the top: its original nodes are the four at y = 2.
    uint32_t top = 0;
    for (const GeomNode& node : result.positive.nodes)
        if (node.cutId == 0)
        {
            ++top;
            CHECK(node.position.y == 2.0f);
        }
    CHECK(top == 4);
    // One outer loop of 8 nodes around a 2 x 2 square, counter-clockwise in the plane basis for
    // the positive piece.
    CHECK(result.loops.size() == 1 && result.loops[0].size() == 8);
    CHECK(result.loopDepth.size() == 1 && result.loopDepth[0] == 0);
    CHECK(result.loopArea.size() == 1 && std::fabs(result.loopArea[0] - 4.0) < 1e-5);
    // The input is untouched.
    CHECK(box.nodes.size() == 8 && box.faces.size() == 12);
}

// Case 2: the plane passes through nodes (the octahedron equator). No new nodes, no zero-area
// faces, and the four equator edges become the boundary of both pieces.
static void TestPlaneThroughNodes()
{
    const PatchPiece octa = MakeOctahedron();
    SliceResult result;
    const SliceStatus status = SlicePatch(octa, MakePlane(0, 0, 0, 0, 1, 0), 1, NoCapOptions(), result);
    CHECK_STATUS(status, SliceStatus::Ok, result.detail);
    CHECK(result.positive.nodes.size() == 5 && result.negative.nodes.size() == 5);
    CHECK(CountCutNodes(result.positive, 1) == 0 && CountCutNodes(result.negative, 1) == 0);
    CHECK(result.positive.faces.size() == 4 && result.negative.faces.size() == 4);
    CHECK(CountKind(result.positive, EdgeKind::Cut) == 4 && CountKind(result.negative, EdgeKind::Cut) == 4);
    CheckPieceValid(result.positive, "octahedron +");
    CheckPieceValid(result.negative, "octahedron -");
    // The loop is the equator diamond: 4 existing nodes, area 2.
    CHECK(result.loops.size() == 1 && result.loops[0].size() == 4);
    CHECK(result.loopArea.size() == 1 && std::fabs(result.loopArea[0] - 2.0) < 1e-6);
}

// A plane through exactly one node: the faces around it split the opposite edge ((0, +, -) faces)
// or keep a lone corner ((+, -, -) faces), and the cut loop passes through the existing node.
static void TestOneNodeOnPlane()
{
    const PatchPiece octa = MakeOctahedron();
    SliceResult result;
    const SliceStatus status = SlicePatch(octa, MakePlane(1, 0, 0, 1, 2, 0), 1, NoCapOptions(), result);
    CHECK_STATUS(status, SliceStatus::Ok, result.detail);
    CHECK(CountCutNodes(result.positive, 1) == 3);      // edges apex-e1, apex-e2, apex-e3
    CHECK(result.loops.size() == 1 && result.loops[0].size() == 4);     // those 3 + the node on the plane
    CHECK(CountKind(result.positive, EdgeKind::Cut) == 4 && CountKind(result.negative, EdgeKind::Cut) == 4);
    CheckPieceValid(result.positive, "one node +");
    CheckPieceValid(result.negative, "one node -");
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
    PatchPiece grid = MakePiece(p, t);
    TagOpenEdges(grid, EdgeKind::Replacement);
    SliceResult result;
    CHECK_STATUS(SlicePatch(grid, MakePlane(1.5f, 0, 0, 1, 0, 0), 1, NoCapOptions(), result), SliceStatus::OpenLoop, result.detail);
    CHECK(result.detail.find("Replacement") != std::string::npos);
    CHECK(result.positive.Empty() && result.negative.Empty() && result.loops.empty());

    // A single triangle cut through one corner is open as well.
    PatchPiece tri = MakePiece({ { 0, 0, 0 }, { 1, 1, 0 }, { 1, -1, 0 } }, { { 0, 1, 2 } });
    TagOpenEdges(tri, EdgeKind::Replacement);
    CHECK_STATUS(SlicePatch(tri, MakePlane(0, 0, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::OpenLoop, result.detail);
}

// Case 3: an edge lies on the plane and both faces around it are on the same side (touching).
static void TestTouchingIsNoCut()
{
    const std::vector<XMFLOAT3> p = { { 0, 0, 0 }, { 1, 0, 0 }, { 0.5f, 1, 0.5f }, { 0.5f, 1, -0.5f } };
    const XMFLOAT3 center = { 0.5f, 0.5f, 0.0f };
    const std::vector<Tri> t = { OutwardTri(p, { 0, 1, 2 }, center), OutwardTri(p, { 0, 3, 1 }, center),
        OutwardTri(p, { 0, 2, 3 }, center), OutwardTri(p, { 1, 3, 2 }, center) };
    const PatchPiece tet = MakePiece(p, t);
    SliceResult result;
    const SliceStatus status = SlicePatch(tet, MakePlane(0, 0, 0, 0, 1, 0), 1, NoCapOptions(), result);
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
    const PatchPiece tet = MakePiece(p, t);
    SliceResult result;
    const SliceStatus status = SlicePatch(tet, MakePlane(0, 0, 0, 0, 1, 0), 1, NoCapOptions(), result);
    CHECK_STATUS(status, SliceStatus::CoplanarFace, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());
}

// Case 5: the plane misses the piece.
static void TestMissIsNoCut()
{
    const PatchPiece box = MakeBox();
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 10, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::NoCut, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty() && result.loops.empty());
    CHECK_STATUS(SlicePatch(box, MakePlane(0, -1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::NoCut, result.detail);
}

// Case 12: failures leave nothing but status and detail, even when out held a previous result.
static void TestFailureClearsOut()
{
    const PatchPiece box = MakeBox();
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(!result.positive.Empty() && !result.negative.Empty());

    result.loops.push_back({ 1, 2, 3 });
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 10, 0, 0, 1, 0), 2, NoCapOptions(), result), SliceStatus::NoCut, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty() && result.loops.empty());
    CHECK(result.positive.nodes.empty() && result.negative.corners.empty());
    CHECK(!result.detail.empty());

    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 0, 0), 1, NoCapOptions(), result), SliceStatus::InvalidInput, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());

    PatchPiece broken = box;
    broken.faces[3].corner[0] = 99;
    CHECK_STATUS(SlicePatch(broken, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::InvalidInput, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());
}

// Position bits come from the node: every corner of one node bakes to the same position.
static void TestBakeSharesNodePositions()
{
    PatchPiece strip = MakePiece({ { 0, 0, 0 }, { 0, 2, 0 }, { 1, 1, 0 }, { -1, 1, 0 } }, { { 0, 1, 2 }, { 1, 0, 3 } });
    RenderCorner a2 = strip.corners[0];
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
    strip.faces[1].kind = FaceKind::Cap;
    BuildRenderMesh(strip, baked);
    CHECK(baked.sections.size() == 2 && baked.sections[0].indexCount == 3 && baked.sections[1].firstIndex == 3
        && baked.sections[1].indexCount == 3 && baked.sections[1].materialSlot == 1);
    ProceduralMesh mesh;
    std::string error;
    CHECK(mesh.SetMesh(baked, &error));
}

// A cut node may depend on at most three original nodes.
static void TestOriginLimit()
{
    PatchPiece box = MakeBox();
    for (int i = 0; i < 3; ++i)
    {
        box.nodes[0].origin.node[i] = uint32_t(i);
        box.nodes[0].origin.bary[i] = 1.0f / 3.0f;
    }
    box.nodes[4].origin.node[0] = 3;
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::InvalidOrigin, result.detail);
    CHECK(result.positive.Empty());
}

// The plane may never split a cap face.
static void TestCapFaceIsNotCrossed()
{
    PatchPiece box = MakeBox();
    box.faces[4].kind = FaceKind::Cap;
    box.faces[4].cutId = 7;
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::CrossesCap, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());
}

// A tilted plane splits the box into two non-empty pieces whose areas still add up.
static void TestTiltedCutKeepsArea()
{
    const PatchPiece box = MakeBox();
    SliceResult result;
    const SliceStatus status = SlicePatch(box, MakePlane(0.1f, 0.9f, -0.2f, 0.3f, 1.0f, 0.2f), 1, NoCapOptions(), result);
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
    PatchPiece piece = MakePiece(p, t);
    TagOpenEdges(piece, EdgeKind::Replacement);
    std::string detail;
    CHECK_STATUS(ValidatePatchPiece(piece, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);

    SliceResult result;
    CHECK_STATUS(SlicePatch(piece, MakePlane(20.5f, 0, 0, 1, 0, 0), 1, NoCapOptions(), result),
        SliceStatus::DegenerateInput, result.detail);
    CHECK(result.positive.Empty() && result.negative.Empty());
    CHECK(result.detail.find("output") == 0);

    // The same cut on a triangle that is not marginal is fine.
    piece.nodes[6].position.y = 1e-3f;
    // The sheet is open, so the cut ends as OpenLoop: reaching the loop stage shows the output
    // pieces passed validation (a sliver would have stopped earlier with DegenerateInput).
    CHECK_STATUS(SlicePatch(piece, MakePlane(20.5f, 0, 0, 1, 0, 0), 1, NoCapOptions(), result), SliceStatus::OpenLoop, result.detail);
}

// ---------------------------------------------------------------- 2-3 tests

// Appends src to dst (nodes, corners, faces, origin nodes and edge tags are renumbered).
static void AppendPiece(PatchPiece& dst, const PatchPiece& src)
{
    const uint32_t nodeBase = uint32_t(dst.nodes.size());
    const uint32_t cornerBase = uint32_t(dst.corners.size());
    const uint32_t originBase = uint32_t(dst.originNodes.size());
    for (const SkinNode& skin : src.originNodes)
        dst.originNodes.push_back(skin);
    for (GeomNode node : src.nodes)
    {
        for (int i = 0; i < 3; ++i)
            if (node.origin.node[i] != kNoNode)
                node.origin.node[i] += originBase;
        dst.nodes.push_back(node);
    }
    for (RenderCorner corner : src.corners)
    {
        corner.node += nodeBase;
        dst.corners.push_back(corner);
    }
    for (Face face : src.faces)
    {
        for (int k = 0; k < 3; ++k)
            face.corner[k] += cornerBase;
        dst.faces.push_back(face);
    }
    for (const auto& entry : src.boundary)
        dst.boundary[MakeEdgeKey(EdgeKeyLow(entry.first) + nodeBase, EdgeKeyHigh(entry.first) + nodeBase)] = entry.second;
}

// The box scaled to a square of half-size `half` around (cx, cz) with the given top height.
static PatchPiece MakeBoxAt(float cx, float cz, float half, float height)
{
    PatchPiece box = MakeBox();
    for (GeomNode& node : box.nodes)
    {
        node.position.x = cx + node.position.x * half;
        node.position.z = cz + node.position.z * half;
        node.position.y = node.position.y * 0.5f * height;
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
static PatchPiece MakeThickTube(int n, float outer, float inner)
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

// Case 7: a UV seam (separate corners, one node per column after welding) is cut without a
// break. Seam corners keep their UVs and share the node's position bits.
// Cut attributes run from the lower node to the higher one whichever way a face walks the edge:
// at y = 0.5 (t = 0.25 up the wall) every cut corner has v = 0.25.
static void TestUvSeamTubeCut()
{
    PatchPiece tube;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildPatchPiece(MakeTubeWeldInput(8, true, 0), kDefaultEpsilonScale, tube, stats, &detail), SliceStatus::Ok, detail);
    SliceResult result;
    CHECK_STATUS(SlicePatch(tube, MakePlane(0, 0.5f, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.size() == 1 && result.loops[0].size() == 16);       // 8 vertical edges + 8 diagonals
    CHECK(result.loopDepth.size() == 1 && result.loopDepth[0] == 0);
    CheckPieceValid(result.positive, "tube +");
    CheckPieceValid(result.negative, "tube -");

    int seamLow = 0, seamHigh = 0;
    for (const RenderCorner& corner : result.positive.corners)
    {
        if (result.positive.nodes[corner.node].cutId != 1)
            continue;
        CHECK(std::fabs(corner.uv.y - 0.25f) < 1e-5f);
        const XMFLOAT3& pos = result.positive.nodes[corner.node].position;
        if (std::fabs(pos.x - 1.0f) < 1e-6f && std::fabs(pos.z) < 1e-6f)
        {
            seamLow += std::fabs(corner.uv.x) < 1e-6f;
            seamHigh += std::fabs(corner.uv.x - 1.0f) < 1e-6f;
        }
    }
    CHECK(seamLow == 1 && seamHigh == 1);    // one corner per side of the seam, both on one node

    ProceduralMeshData baked;
    BuildRenderMesh(result.positive, baked);
    bool bitEqual = false;
    for (size_t i = 0; i < baked.vertices.size(); ++i)
        for (size_t j = i + 1; j < baked.vertices.size(); ++j)
            if (result.positive.corners[i].node == result.positive.corners[j].node
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
    PatchPiece tube;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildPatchPiece(MakeTubeWeldInput(8, true, 1), kDefaultEpsilonScale, tube, stats, &detail), SliceStatus::Ok, detail);
    SliceResult result;
    CHECK_STATUS(SlicePatch(tube, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::OpenLoop, result.detail);
    CHECK(result.detail.find("UnweldedSeam") != std::string::npos);
}

// Case 9: a thick tube cut across gives an outer loop and a hole. The hole has the opposite
// winding and sits inside the outer loop.
static void TestThickTubeHoleLoops()
{
    const PatchPiece tube = MakeThickTube(8, 2.0f, 1.0f);
    std::string detail;
    CHECK_STATUS(ValidatePatchPiece(tube, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    SliceResult result;
    CHECK_STATUS(SlicePatch(tube, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::Ok, result.detail);
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

// Two solids that touch at one node: that node has two outgoing and two incoming cut edges.
static void TestSharedNodeIsNonManifoldLoop()
{
    std::vector<XMFLOAT3> p = { { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 }, { 0, 0, -1 }, { 0, 1, 0 }, { 0, -1, 0 } };
    std::vector<Tri> t;
    for (uint32_t i = 0; i < 4; ++i)
    {
        t.push_back(OutwardTri(p, { 4, i, (i + 1) % 4 }, { 0, 0, 0 }));
        t.push_back(OutwardTri(p, { 5, i, (i + 1) % 4 }, { 0, 0, 0 }));
    }
    // Second octahedron centered at (2,0,0): its node at (1,0,0) is node 0 of the first.
    const uint32_t b[6] = { 6, 7, 0, 8, 9, 10 };        // local 0..5 -> global
    const XMFLOAT3 local[6] = { { 3, 0, 0 }, { 2, 0, 1 }, { 1, 0, 0 }, { 2, 0, -1 }, { 2, 1, 0 }, { 2, -1, 0 } };
    p.push_back(local[0]); p.push_back(local[1]); p.push_back(local[3]); p.push_back(local[4]); p.push_back(local[5]);
    for (uint32_t i = 0; i < 4; ++i)
    {
        t.push_back(OutwardTri(p, { b[4], b[i], b[(i + 1) % 4] }, { 2, 0, 0 }));
        t.push_back(OutwardTri(p, { b[5], b[i], b[(i + 1) % 4] }, { 2, 0, 0 }));
    }
    const PatchPiece pair = MakePiece(p, t);
    std::string detail;
    CHECK_STATUS(ValidatePatchPiece(pair, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    SliceResult result;
    CHECK_STATUS(SlicePatch(pair, MakePlane(0, 0, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::NonManifoldLoop, result.detail);
}

// Loops that cross, nest with the wrong winding, or nest too deep are InvalidLoop.
static void TestInvalidLoops()
{
    SliceResult result;

    PatchPiece crossing = MakeBoxAt(0, 0, 1, 2);
    AppendPiece(crossing, MakeBoxAt(1, 1, 1, 2));
    CHECK_STATUS(SlicePatch(crossing, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::InvalidLoop, result.detail);
    CHECK(result.detail.find("cross") != std::string::npos);

    // A box inside a box with the same orientation: the inner loop is not a hole.
    PatchPiece nested = MakeBoxAt(0, 0, 3, 2);
    AppendPiece(nested, MakeBoxAt(0, 0, 1, 2));
    CHECK_STATUS(SlicePatch(nested, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::InvalidLoop, result.detail);
    CHECK(result.detail.find("wrong way") != std::string::npos);

    // An island inside the hole of a thick tube is nested two deep.
    PatchPiece island = MakeThickTube(8, 3.0f, 2.0f);
    AppendPiece(island, MakeBoxAt(0, 0, 0.5f, 2));
    CHECK_STATUS(SlicePatch(island, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::InvalidLoop, result.detail);
    CHECK(result.detail.find("nested") != std::string::npos);

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
    const PatchPiece bowtie = MakePiece(p, t);
    std::string detail;
    CHECK_STATUS(ValidatePatchPiece(bowtie, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    CHECK_STATUS(SlicePatch(bowtie, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::InvalidLoop, result.detail);
    CHECK(result.detail.find("itself") != std::string::npos);
}

// ---------------------------------------------------------------- 2-4 tests

static SliceOptions CapOptions()
{
    SliceOptions options;
    options.cap = CapMode::Create;
    return options;
}

static uint32_t CountOpenEdges(const PatchPiece& piece)
{
    EdgeUsageMap usage;
    CollectEdgeUsage(piece, usage);
    uint32_t n = 0;
    for (const auto& entry : usage)
        n += entry.second.count == 1;
    return n;
}

static uint32_t CountCapFaces(const PatchPiece& piece)
{
    uint32_t n = 0;
    for (const Face& face : piece.faces)
        n += face.kind == FaceKind::Cap;
    return n;
}

// Dot of a cap face's geometric normal with a direction.
static double CapFacing(const PatchPiece& piece, double dx, double dy, double dz)
{
    for (const Face& face : piece.faces)
    {
        if (face.kind != FaceKind::Cap)
            continue;
        const XMFLOAT3 &a = piece.nodes[piece.corners[face.corner[0]].node].position;
        const XMFLOAT3 &b = piece.nodes[piece.corners[face.corner[1]].node].position;
        const XMFLOAT3 &c = piece.nodes[piece.corners[face.corner[2]].node].position;
        const double e1[3] = { double(b.x) - a.x, double(b.y) - a.y, double(b.z) - a.z };
        const double e2[3] = { double(c.x) - a.x, double(c.y) - a.y, double(c.z) - a.z };
        return (e1[1] * e2[2] - e1[2] * e2[1]) * dx + (e1[2] * e2[0] - e1[0] * e2[2]) * dy + (e1[0] * e2[1] - e1[1] * e2[0]) * dz;
    }
    return 0.0;
}

// Case 10: the cap is watertight. The rim has collinear nodes (the diagonal crossings sit in the
// middle of each square side), which an ear clipper that drops straight points would turn into
// T-junctions; here every cut edge is shared with exactly one cap face.
static void TestCapIsWatertight()
{
    const PatchPiece box = MakeBox();
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CheckPieceValid(result.positive, "capped box +");
    CheckPieceValid(result.negative, "capped box -");
    CHECK(CountCapFaces(result.positive) == 6 && CountCapFaces(result.negative) == 6);   // 8-gon rim
    CHECK(CountKind(result.positive, EdgeKind::Cut) == 0 && CountKind(result.negative, EdgeKind::Cut) == 0);
    CHECK(CountKind(result.positive, EdgeKind::CapSeam) == 8 && CountKind(result.negative, EdgeKind::CapSeam) == 8);
    CHECK(CountOpenEdges(result.positive) == 0 && CountOpenEdges(result.negative) == 0);   // closed solids
    CHECK(std::fabs(TotalArea(result.positive, FaceKind::Cap) - 4.0) < 1e-5);
    // The cap faces away from the kept side.
    CHECK(CapFacing(result.positive, 0, -1, 0) > 0.0 && CapFacing(result.negative, 0, 1, 0) > 0.0);
    CHECK(result.positive.caps.size() == 1 && result.positive.caps[0].rimNodes.size() == 8 && result.positive.caps[0].cutId == 1);
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
    for (const Face& face : result.positive.faces)
        if (face.kind == FaceKind::Cap)
            for (int k = 0; k < 3; ++k)
                CHECK(result.positive.corners[face.corner[k]].normal.y == -1.0f);
}

// A tube with a hole: the cap is the outer polygon minus the hole, joined by bridge edges.
static void TestCapWithHole()
{
    const PatchPiece tube = MakeThickTube(8, 2.0f, 1.0f);
    SliceResult result;
    CHECK_STATUS(SlicePatch(tube, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.size() == 2);
    CheckPieceValid(result.positive, "capped tube +");
    CheckPieceValid(result.negative, "capped tube -");
    CHECK(CountCapFaces(result.positive) == 32 && CountCapFaces(result.negative) == 32);   // 16 + 16 + 2 holes - 2
    CHECK(CountOpenEdges(result.positive) == 0 && CountOpenEdges(result.negative) == 0);
    if (result.loopArea.size() == 2)
    {
        const double expected = std::fabs(result.loopArea[0]) - std::fabs(result.loopArea[1]);
        CHECK(std::fabs(std::fabs(TotalArea(result.positive, FaceKind::Cap)) - std::fabs(expected)) < 1e-4 * std::fabs(expected));
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
    const PatchPiece prism = MakePiece(p, t);
    std::string detail;
    CHECK_STATUS(ValidatePatchPiece(prism, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    SliceResult result;
    CHECK_STATUS(SlicePatch(prism, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CheckPieceValid(result.positive, "L prism +");
    CheckPieceValid(result.negative, "L prism -");
    CHECK(std::fabs(TotalArea(result.positive, FaceKind::Cap) - 3.0) < 1e-5);        // the L covers 3 unit squares
    CHECK(CountOpenEdges(result.positive) == 0 && CountOpenEdges(result.negative) == 0);
}

// Case 11: cutting a piece that already has a cap. A cut through the surface only is fine and the
// old cap and its seam tags stay with the piece that keeps them; a cut through the cap is rejected.
static void TestRecut()
{
    const PatchPiece box = MakeBox();
    SliceResult first;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), first), SliceStatus::Ok, first.detail);
    const PatchPiece upper = first.positive;       // y in [1, 2], cap at y = 1

    SliceResult second;
    CHECK_STATUS(SlicePatch(upper, MakePlane(0, 1.5f, 0, 0, 1, 0), 2, CapOptions(), second), SliceStatus::Ok, second.detail);
    CheckPieceValid(second.positive, "recut +");
    CheckPieceValid(second.negative, "recut -");
    // The slab between the two cuts keeps the first cap; the top part has none of it.
    CHECK(second.negative.caps.size() == 2 && second.positive.caps.size() == 1);
    CHECK(second.positive.caps[0].cutId == 2 && second.negative.caps[0].cutId == 1);
    CHECK(second.loops.size() == 1);
    const size_t newSeam = second.loops.empty() ? 0 : second.loops[0].size();
    CHECK(CountKind(second.negative, EdgeKind::CapSeam) == 8 + newSeam);    // inherited old seam + new seam
    CHECK(CountKind(second.positive, EdgeKind::CapSeam) == newSeam);
    CHECK(CountOpenEdges(second.negative) == 0 && CountOpenEdges(second.positive) == 0);
    for (const GeomNode& node : second.negative.nodes)
    {
        int used = 0;
        double sum = 0.0;
        for (int i = 0; i < 3; ++i)
            if (node.origin.node[i] != kNoNode)
            {
                ++used;
                sum += node.origin.bary[i];
            }
        CHECK(used >= 1 && used <= 3 && std::fabs(sum - 1.0) < 1e-4);
    }

    // A plane that misses the piece, and one that cuts through the cap.
    SliceResult miss;
    CHECK_STATUS(SlicePatch(upper, MakePlane(0, 0.5f, 0, 0, 1, 0), 2, CapOptions(), miss), SliceStatus::NoCut, miss.detail);
    SliceResult through;
    CHECK_STATUS(SlicePatch(upper, MakePlane(0, 1.5f, 0, 1, 0, 0), 2, CapOptions(), through), SliceStatus::CrossesCap, through.detail);
    CHECK(through.positive.Empty() && through.negative.Empty());
    // The input piece is untouched by all of this.
    CHECK(upper.caps.size() == 1 && CountKind(upper, EdgeKind::CapSeam) == 8);
}

// ---------------------------------------------------------------- debug tests (face origin)

// A surface face carries the cut that divided it; a face moved over whole keeps its old value.
// The octahedron cut through one node has (0, +, -) faces, whose opposite edge is divided.
static void CheckDividedFacesHaveCutId(const SliceResult& result, const PatchPiece& input, uint32_t cutId, const char* label)
{
    // Per source triangle: faces going in, faces coming out (both pieces). More faces out than in
    // means this cut divided one of them; otherwise every face keeps a cutId it already had.
    std::vector<uint32_t> in(input.faces.size() + 64, 0), out(in.size(), 0);
    for (const Face& face : input.faces)
        if (face.kind == FaceKind::Surface)
            ++in[face.sourceTriangle];
    for (const PatchPiece* piece : { &result.positive, &result.negative })
        for (const Face& face : piece->faces)
            if (face.kind == FaceKind::Surface)
                ++out[face.sourceTriangle];
    uint32_t divided = 0;
    for (uint32_t t = 0; t < in.size(); ++t)
    {
        if (out[t] == 0)
            continue;
        const bool wasDivided = out[t] > in[t];
        bool marked = false;
        for (const PatchPiece* piece : { &result.positive, &result.negative })
            for (const Face& face : piece->faces)
            {
                if (face.kind != FaceKind::Surface || face.sourceTriangle != t)
                    continue;
                marked = marked || face.cutId == cutId;
                bool known = face.cutId == cutId && wasDivided;
                for (const Face& old : input.faces)
                    known = known || (old.kind == FaceKind::Surface && old.sourceTriangle == t && old.cutId == face.cutId);
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
    // Plane through one node: (0, +, -) faces and lone-corner faces.
    const PatchPiece octa = MakeOctahedron();
    SliceResult oneNode;
    CHECK_STATUS(SlicePatch(octa, MakePlane(1, 0, 0, 1, 2, 0), 1, NoCapOptions(), oneNode), SliceStatus::Ok, oneNode.detail);
    CheckDividedFacesHaveCutId(oneNode, octa, 1, "octahedron");

    // Nothing was cut before, so every face is either undivided (0) or divided by cut 1.
    for (const PatchPiece* piece : { &oneNode.positive, &oneNode.negative })
        for (const Face& face : piece->faces)
            CHECK(face.cutId == 0 || face.cutId == 1);

    // Second cut: cut 2 marks the faces it divides.
    const PatchPiece box = MakeBox();
    SliceResult first, second;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 0.5f, 0, 0.3f, 1, 0), 1, CapOptions(), first), SliceStatus::Ok, first.detail);
    CheckDividedFacesHaveCutId(first, box, 1, "box cut 1");
    CHECK_STATUS(SlicePatch(first.positive, MakePlane(0, 1.5f, 0, 0, 1, 0.2f), 2, CapOptions(), second), SliceStatus::Ok, second.detail);
    CheckDividedFacesHaveCutId(second, first.positive, 2, "box cut 2");
    // A face moved over whole keeps the value it had (top face triangle 2 is untouched by a cut at y = 1).
    PatchPiece marked = MakeBox();
    marked.faces[2].cutId = 7;
    SliceResult keep;
    CHECK_STATUS(SlicePatch(marked, MakePlane(0, 1, 0, 0, 1, 0), 8, NoCapOptions(), keep), SliceStatus::Ok, keep.detail);
    uint32_t sevens = 0;
    for (const Face& face : keep.positive.faces)
        sevens += face.kind == FaceKind::Surface && face.sourceTriangle == 2 && face.cutId == 7;
    CHECK(sevens == 1);
    // Cap faces keep meaning "the cut that made the cap".
    for (const Face& face : second.negative.faces)
        if (face.kind == FaceKind::Cap)
            CHECK(face.cutId == 1 || face.cutId == 2);
}

static void TestSplitByOriginBake()
{
    const PatchPiece box = MakeBox();
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 0.5f, 0, 0.3f, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);

    RenderBakeOptions split;
    split.splitByOrigin = true;
    for (const PatchPiece* piece : { &result.positive, &result.negative })
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
        for (const Face& face : piece->faces)
            if (face.kind == FaceKind::Surface)
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
    const PatchPiece box = MakeBox();
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.loops.size() == 1);
    ProceduralMeshData tubes;
    BakeLoopDebug(result.positive, result.loops, 0.02f, { 1.0f, 0.2f, 1.0f, 1.0f }, tubes);
    const size_t edges = result.loops.empty() ? 0 : result.loops[0].size();
    CHECK(edges == 8 && tubes.vertices.size() == edges * 6 && tubes.indices.size() == edges * 18);
    CHECK(tubes.sections.size() == 1 && tubes.sections[0].indexCount == tubes.indices.size());
    // The tubes sit on the cut plane (y = 1), around the cut loop nodes.
    for (const ProceduralVertex& vertex : tubes.vertices)
        CHECK(std::fabs(vertex.position.y - 1.0f) <= 0.02f + 1e-5f);
    ProceduralMesh mesh;
    std::string error;
    CHECK(mesh.SetMesh(tubes, &error));

    // No loops, or loops that name missing nodes: nothing to draw.
    BakeLoopDebug(result.positive, {}, 0.02f, { 1.0f, 1.0f, 1.0f, 1.0f }, tubes);
    CHECK(tubes.vertices.empty() && tubes.sections.empty());
    BakeLoopDebug(result.positive, { { 0, 9999 } }, 0.02f, { 1.0f, 1.0f, 1.0f, 1.0f }, tubes);
    CHECK(tubes.indices.empty() && tubes.sections.empty());
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

// Two moderate, different rigid motions: skinning a node between them shears the box a little.
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

// A closed box skinned by joints 0 and 1: weight of joint 1 grows with height. The nodes are
// posed with `slots`, so the piece is consistent with that pose.
static PatchPiece MakeSkinnedBox(const SlotMatrices& slots)
{
    PatchPiece box = MakeBox();
    for (size_t i = 0; i < box.nodes.size(); ++i)
    {
        SkinNode& skin = box.originNodes[i];
        skin.bindPosition = box.nodes[i].position;
        skin.jointCount = 2;
        skin.joints[0] = 0;
        skin.joints[1] = 1;
        const float w1 = box.nodes[i].position.y * 0.5f * 0.8f + 0.1f;      // 0.1 .. 0.9
        skin.weights[0] = 1.0f - w1;
        skin.weights[1] = w1;
    }
    SkinStats stats;
    std::string detail;
    const bool ok = SkinPatchPiece(box, slots, &stats, &detail);
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        std::printf("FAIL MakeSkinnedBox: %s\n", detail.c_str());
    }
    return box;
}

static double MaxNodeDelta(const PatchPiece& a, const PatchPiece& b)
{
    double worst = 0.0;
    if (a.nodes.size() != b.nodes.size())
        return 1e30;
    for (size_t i = 0; i < a.nodes.size(); ++i)
    {
        const double dx = double(a.nodes[i].position.x) - b.nodes[i].position.x;
        const double dy = double(a.nodes[i].position.y) - b.nodes[i].position.y;
        const double dz = double(a.nodes[i].position.z) - b.nodes[i].position.z;
        worst = std::max(worst, std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    return worst;
}

static bool SamePosition(const XMFLOAT3& a, const XMFLOAT3& b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

// 1. Identity matrices put every original node at its bind position, whatever it held before.
static void TestSkinIdentity()
{
    PatchPiece box = MakeBox();
    const PatchPiece reference = box;
    for (GeomNode& node : box.nodes)
        node.position = { node.position.x + 100.0f, -7.0f, 3.0f };
    SkinStats stats;
    std::string detail;
    CHECK(SkinPatchPiece(box, IdentitySlots(1), &stats, &detail));
    CHECK(MaxNodeDelta(box, reference) == 0.0);
    CHECK(stats.originNodesEvaluated == 8 && stats.surfaceNodesWritten == 8 && stats.capNodesSkipped == 0);
}

// 2. One joint with a rotation and a translation: bind * R * T, row-vector convention.
static void TestSkinSingleJoint()
{
    PatchPiece tri = MakePiece({ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { { 0, 1, 2 } });
    for (SkinNode& skin : tri.originNodes)
        skin.joints[0] = 1;
    SlotMatrices slots = IdentitySlots(2);
    slots[0][1] = ToFloat4x4(DirectX::XMMatrixRotationZ(1.5707963267948966f) * DirectX::XMMatrixTranslation(5.0f, 0.0f, 0.0f));
    std::string detail;
    CHECK(SkinPatchPiece(tri, slots, nullptr, &detail));
    const XMFLOAT3 expected[3] = { { 5, 1, 0 }, { 4, 0, 0 }, { 5, 0, 1 } };    // (1,0,0)->(0,1,0)+5x, ...
    for (int i = 0; i < 3; ++i)
        CHECK(std::fabs(tri.nodes[i].position.x - expected[i].x) < 1e-5f && std::fabs(tri.nodes[i].position.y - expected[i].y) < 1e-5f
            && std::fabs(tri.nodes[i].position.z - expected[i].z) < 1e-5f);
}

// 3. Two joints with weights 0.25 / 0.75, computed by hand:
//    0.25 * (5,1,1) + 0.75 * (1,9,1) = (2,7,1).
static void TestSkinTwoJointBlend()
{
    PatchPiece tri = MakePiece({ { 1, 1, 1 }, { 2, 2, 2 }, { 3, 3, 3 } }, { { 0, 1, 2 } });
    SkinNode& skin = tri.originNodes[0];
    skin.jointCount = 2;
    skin.joints[0] = 0;
    skin.joints[1] = 1;
    skin.weights[0] = 0.25f;
    skin.weights[1] = 0.75f;
    SlotMatrices slots(1);
    slots[0].push_back(ToFloat4x4(DirectX::XMMatrixTranslation(4.0f, 0.0f, 0.0f)));
    slots[0].push_back(ToFloat4x4(DirectX::XMMatrixTranslation(0.0f, 8.0f, 0.0f)));
    CHECK(SkinPatchPiece(tri, slots));
    CHECK(std::fabs(tri.nodes[0].position.x - 2.0f) < 1e-6f && std::fabs(tri.nodes[0].position.y - 7.0f) < 1e-6f
        && std::fabs(tri.nodes[0].position.z - 1.0f) < 1e-6f);
    // The same blend straight from SkinPoint.
    XMFLOAT3 point;
    DirectX::XMStoreFloat3(&point, SkinPoint(slots[0].data(), 2, skin.joints, skin.weights, 2, skin.bindPosition));
    CHECK(point.x == tri.nodes[0].position.x && point.y == tri.nodes[0].position.y && point.z == tri.nodes[0].position.z);
}

// 4. A piece cut in a pose and skinned with the same matrices again lands on the cut positions:
//    cut nodes are driven by their SurfaceOrigin, with no joint or weight of their own.
static void TestSkinMatchesSliceNodes()
{
    const SlotMatrices pose = PoseA();
    const PatchPiece box = MakeSkinnedBox(pose);
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 2.0f, 0, 0.3f, 1.0f, 0.2f), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    for (PatchPiece* piece : { &result.positive, &result.negative })
    {
        PatchPiece skinned = *piece;
        for (GeomNode& node : skinned.nodes)
            node.position = { 0.0f, 0.0f, 0.0f };
        SkinStats stats;
        std::string detail;
        CHECK(SkinPatchPiece(skinned, pose, &stats, &detail));
        const double tolerance = 1e-5 * ComputePieceRadius(*piece);
        CHECK(MaxNodeDelta(skinned, *piece) <= tolerance);
        CHECK(stats.capNodesSkipped == 0);
        CHECK(stats.originNodesEvaluated <= box.originNodes.size());
    }
}

// 5. Seam corners and cap rim corners follow their node: after a pose change every corner of a node
//    bakes to the same bits. Cap faces use no nodes of their own.
static void TestSkinKeepsSeamsAndCapsTogether()
{
    PatchPiece tube;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildPatchPiece(MakeTubeWeldInput(8, true, 0), kDefaultEpsilonScale, tube, stats, &detail), SliceStatus::Ok, detail);
    SliceResult result;
    CHECK_STATUS(SlicePatch(tube, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);

    for (PatchPiece* piece : { &result.positive, &result.negative })
    {
        for (const GeomNode& node : piece->nodes)
            CHECK(node.kind == NodeKind::Surface);          // no cap-only nodes exist
        ProceduralMeshData baked;
        BuildRenderMesh(*piece, baked);
        const SlotMatrices pose = PoseA();
        SkinStats skinStats;
        CHECK(SkinPatchPiece(*piece, pose, &skinStats, &detail));
        CHECK(skinStats.capNodesSkipped == 0);
        CHECK(UpdateRenderPositions(*piece, baked.vertices));

        std::vector<int> firstCorner(piece->nodes.size(), -1);
        bool seamSeen = false, capSeen = false;
        for (size_t c = 0; c < piece->corners.size(); ++c)
        {
            const uint32_t node = piece->corners[c].node;
            if (firstCorner[node] < 0)
            {
                firstCorner[node] = int(c);
                continue;
            }
            seamSeen = true;                                // a node with more than one corner (UV seam or cap)
            CHECK(SamePosition(baked.vertices[c].position, baked.vertices[firstCorner[node]].position));
            CHECK(SamePosition(baked.vertices[c].position, piece->nodes[node].position));
        }
        for (const Face& face : piece->faces)
            capSeen = capSeen || face.kind == FaceKind::Cap;
        CHECK(seamSeen && capSeen);
    }
}

// 6. A piece with its own skin state (separated bones) is refused and left alone.
static void TestSkinRefusesSkinState()
{
    PatchPiece box = MakeBox();
    box.skinStateId = 3;
    const PatchPiece before = box;
    std::string detail;
    CHECK(!SkinPatchPiece(box, IdentitySlots(1), nullptr, &detail));
    CHECK(detail.find("skin state") != std::string::npos);
    CHECK(MaxNodeDelta(box, before) == 0.0);
}

// 7. Cutting a piece twice: the second generation skins back to its own positions as well.
static void TestSkinAfterTwoCuts()
{
    const SlotMatrices pose = PoseA();
    const PatchPiece box = MakeSkinnedBox(pose);
    double yLow = 1e30, yHigh = -1e30;
    for (const GeomNode& node : box.nodes)
    {
        yLow = std::min<double>(yLow, node.position.y);
        yHigh = std::max<double>(yHigh, node.position.y);
    }
    SliceResult first, second;
    const float yMid = float((yLow + yHigh) * 0.5);
    CHECK_STATUS(SlicePatch(box, MakePlane(0, yMid, 0, 0, 1, 0), 1, CapOptions(), first), SliceStatus::Ok, first.detail);
    // Cut the upper half again, a quarter of the way up. Only the surface is crossed.
    double upLow = 1e30, upHigh = -1e30;
    for (const GeomNode& node : first.positive.nodes)
    {
        upLow = std::min<double>(upLow, node.position.y);
        upHigh = std::max<double>(upHigh, node.position.y);
    }
    const float yQuarter = float(upLow + (upHigh - upLow) * 0.4);
    CHECK_STATUS(SlicePatch(first.positive, MakePlane(0, yQuarter, 0, 0, 1, 0), 2, CapOptions(), second), SliceStatus::Ok, second.detail);
    for (PatchPiece* piece : { &second.positive, &second.negative })
    {
        PatchPiece skinned = *piece;
        for (GeomNode& node : skinned.nodes)
            node.position = { 9.0f, 9.0f, 9.0f };
        std::string detail;
        CHECK(SkinPatchPiece(skinned, pose, nullptr, &detail));
        CHECK(MaxNodeDelta(skinned, *piece) <= 1e-5 * ComputePieceRadius(*piece));
    }
    // A cut node of the second generation depends on at most three origin nodes.
    for (const GeomNode& node : second.positive.nodes)
        CHECK(node.origin.node[0] != kNoNode);
    // New pose: the second-generation piece moves with the same matrices.
    PatchPiece moved = second.positive;
    CHECK(SkinPatchPiece(moved, PoseB()));
    CHECK(MaxNodeDelta(moved, second.positive) > 0.1);
}

// Bad input is refused without touching the piece.
static void TestSkinRefusesBadInput()
{
    PatchPiece box = MakeSkinnedBox(PoseA());
    const PatchPiece before = box;
    std::string detail;
    CHECK(!SkinPatchPiece(box, IdentitySlots(1), nullptr, &detail));           // joint 1 has no matrix
    CHECK(detail.find("joint") != std::string::npos && MaxNodeDelta(box, before) == 0.0);

    CHECK(!SkinPatchPiece(box, SlotMatrices(), nullptr, &detail));             // no slots at all
    CHECK(detail.find("slot") != std::string::npos && MaxNodeDelta(box, before) == 0.0);

    PatchPiece badOrigin = before;
    badOrigin.nodes[2].origin.node[0] = 99;
    CHECK(!SkinPatchPiece(badOrigin, PoseA(), nullptr, &detail));
    CHECK(detail.find("origin") != std::string::npos && MaxNodeDelta(badOrigin, before) == 0.0);
}

// ---------------------------------------------------------------- main

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
    TestSharedNodeIsNonManifoldLoop();
    TestInvalidLoops();

    TestCapIsWatertight();
    TestCapWithHole();
    TestCapOfConcaveLoop();
    TestRecut();
    TestSplitFacesRecordCutId();
    TestSplitByOriginBake();
    TestLoopDebugBake();

    TestSkinIdentity();
    TestSkinSingleJoint();
    TestSkinTwoJointBlend();
    TestSkinMatchesSliceNodes();
    TestSkinKeepsSeamsAndCapsTogether();
    TestSkinRefusesSkinState();
    TestSkinAfterTwoCuts();
    TestSkinRefusesBadInput();

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures;
}
