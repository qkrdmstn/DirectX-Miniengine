// Standalone tests for the slicing pipeline (Geometry/) and the pure part of the plane gizmo
// (Geometry/CutPlaneGizmo.cpp). No D3D12 device is needed.
//
// Build and run from a Visual Studio developer prompt in MiniEngine/MeshCutting:
//   cl /nologo /EHsc /std:c++14 /W4 /MDd /Fo<outdir>\ /Fe<outdir>\PlaneSlicerTests.exe
//      tests\PlaneSlicerTests.cpp Geometry\*.cpp Rendering\RenderBake.cpp ProceduralMesh.cpp
//   <outdir>\PlaneSlicerTests.exe
// Keep <outdir> outside the repository. The exit code is the number of failed checks.

#include "../Geometry/CutPlane.h"
#include "../Geometry/Patch.h"
#include "../Geometry/Skinning.h"
#include "../Geometry/Slice.h"
#include "../Rendering/RenderBake.h"
#include "../ProceduralMesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
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
static void SetSkinVertices(PatchPiece& piece, std::vector<SkinVertex> vertices)
{
    piece.skin = std::make_shared<const Skinning>(std::move(vertices));
}

static std::vector<SkinVertex> GetSkinVertices(const PatchPiece& piece)
{
    return piece.skin ? piece.skin->GetVertices() : std::vector<SkinVertex>();
}

// One node and one render corner per position, identity skin.
static PatchPiece MakePiece(const std::vector<XMFLOAT3>& positions, const std::vector<Tri>& tris)
{
    PatchPiece piece;
    std::vector<SkinVertex> skinVertices;
    for (size_t i = 0; i < positions.size(); ++i)
    {
        SkinVertex skin;
        skin.bindPosition = positions[i];
        skin.jointCount = 1;
        skin.joints[0] = 0;
        skin.weights[0] = 1.0f;
        skinVertices.push_back(skin);

        PatchNode node;
        node.origin.skinVertex[0] = uint32_t(i);
        node.origin.bary[0] = 1.0f;
        node.position = positions[i];
        piece.nodes.push_back(node);

        PatchCorner corner;
        corner.node = uint32_t(i);
        corner.uv = { positions[i].x, positions[i].z };
        piece.corners.push_back(corner);
    }
    SetSkinVertices(piece, skinVertices);
    for (size_t i = 0; i < tris.size(); ++i)
    {
        PatchFace face;
        for (int k = 0; k < 3; ++k)
            face.corner[k] = tris[i][k];
        face.sourceTriangle = uint32_t(i);
        piece.faces.push_back(face);
    }
    return piece;
}

// Tags every boundary edge of a hand-built piece the way the weld step does for open patches.
static void TagOpenEdges(PatchPiece& piece, PatchEdgeKind kind)
{
    PatchEdgeUsageMap usage;
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
    for (const PatchFace& f : box.faces)
        in.indices.insert(in.indices.end(), { f.corner[0], f.corner[1], f.corner[2] });
    return in;
}

static uint32_t CountKind(const PatchPiece& piece, PatchEdgeKind kind)
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
    CHECK(piece.skin->GetVertices().size() == 8 && piece.nodes[3].origin.skinVertex[0] == 3 && piece.nodes[3].origin.bary[0] == 1.0f);
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
    CHECK(CountKind(piece, PatchEdgeKind::Replacement) == 16);
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
    CHECK(CountKind(piece, PatchEdgeKind::UnweldedSeam) == 2);
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

    PatchPiece badSkinVertex = good;
    badSkinVertex.nodes[3].origin.skinVertex[0] = 99;
    CHECK_STATUS(ValidatePatchPiece(badSkinVertex, kDefaultEpsilonScale, &detail), SliceStatus::InvalidOrigin, detail);

    PatchPiece unsortedOrigin = good;
    unsortedOrigin.nodes[3].origin.skinVertex[0] = 5;
    unsortedOrigin.nodes[3].origin.skinVertex[1] = 3;
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
    fin.nodes.back().origin.skinVertex[0] = 8;
    std::vector<SkinVertex> finSkin = GetSkinVertices(fin);
    finSkin.push_back(finSkin[0]);
    SetSkinVertices(fin, finSkin);
    PatchCorner extra;
    extra.node = 8;
    fin.corners.push_back(extra);
    PatchFace fan;
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
    TagOpenEdges(sheet, PatchEdgeKind::Replacement);
    CHECK_STATUS(ValidatePatchPiece(sheet, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    sheet.boundary[MakeEdgeKey(0, 1)] = PatchEdgeKind::CapSeam;
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
    TagOpenEdges(grid, PatchEdgeKind::Replacement);
    std::string detail;
    CHECK_STATUS(ValidatePatchPiece(grid, kDefaultEpsilonScale, &detail), SliceStatus::Ok, detail);
    CHECK(CountKind(grid, PatchEdgeKind::Replacement) == 12);   // perimeter of a 3x3 sheet
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
    for (const PatchNode& node : piece.nodes)
        if (node.cutId == cutId)
            ++n;
    return n;
}

static double TotalSurfaceArea(const PatchPiece& piece) { return TotalArea(piece, PatchFaceKind::Surface); }

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
    CHECK(CountKind(result.positive, PatchEdgeKind::Cut) == 8 && CountKind(result.negative, PatchEdgeKind::Cut) == 8);
    CHECK(result.positive.faces.size() + result.negative.faces.size() > box.faces.size());
    CheckPieceValid(result.positive, "box +");
    CheckPieceValid(result.negative, "box -");

    // The two pieces cover exactly the original surface.
    const double original = TotalSurfaceArea(box);
    const double sum = TotalSurfaceArea(result.positive) + TotalSurfaceArea(result.negative);
    CHECK(std::fabs(sum - original) < original * 1e-5);

    // Cut nodes lie on the plane and are shared by both pieces (same position bits).
    for (const PatchNode& node : result.positive.nodes)
        if (node.cutId == 1)
            CHECK(std::fabs(node.position.y - 1.0f) < 1e-6f);
    for (const PatchNode& a : result.positive.nodes)
    {
        if (a.cutId != 1)
            continue;
        bool matched = false;
        for (const PatchNode& b : result.negative.nodes)
            matched = matched || (b.cutId == 1 && b.position.x == a.position.x && b.position.y == a.position.y
                && b.position.z == a.position.z);
        CHECK(matched);
    }
    // Positive side keeps the top: its original nodes are the four at y = 2.
    uint32_t top = 0;
    for (const PatchNode& node : result.positive.nodes)
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
    CHECK(CountKind(result.positive, PatchEdgeKind::Cut) == 4 && CountKind(result.negative, PatchEdgeKind::Cut) == 4);
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
    CHECK(CountKind(result.positive, PatchEdgeKind::Cut) == 4 && CountKind(result.negative, PatchEdgeKind::Cut) == 4);
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
    TagOpenEdges(grid, PatchEdgeKind::Replacement);
    SliceResult result;
    CHECK_STATUS(SlicePatch(grid, MakePlane(1.5f, 0, 0, 1, 0, 0), 1, NoCapOptions(), result), SliceStatus::OpenLoop, result.detail);
    CHECK(result.detail.find("Replacement") != std::string::npos);
    CHECK(result.positive.Empty() && result.negative.Empty() && result.loops.empty());

    // A single triangle cut through one corner is open as well.
    PatchPiece tri = MakePiece({ { 0, 0, 0 }, { 1, 1, 0 }, { 1, -1, 0 } }, { { 0, 1, 2 } });
    TagOpenEdges(tri, PatchEdgeKind::Replacement);
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
    PatchCorner a2 = strip.corners[0];
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
    strip.faces[1].kind = PatchFaceKind::Cap;
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
        box.nodes[0].origin.skinVertex[i] = uint32_t(i);
        box.nodes[0].origin.bary[i] = 1.0f / 3.0f;
    }
    box.nodes[4].origin.skinVertex[0] = 3;
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, NoCapOptions(), result), SliceStatus::InvalidOrigin, result.detail);
    CHECK(result.positive.Empty());
}

// The plane may never split a cap face.
static void TestCapFaceIsNotCrossed()
{
    PatchPiece box = MakeBox();
    box.faces[4].kind = PatchFaceKind::Cap;
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
    TagOpenEdges(piece, PatchEdgeKind::Replacement);
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

// src를 dst 뒤에 붙인다 (정점, corner, 면, 스킨 정점, 에지 태그 번호를 다시 매긴다).
static void AppendPiece(PatchPiece& dst, const PatchPiece& src)
{
    const uint32_t nodeBase = uint32_t(dst.nodes.size());
    const uint32_t cornerBase = uint32_t(dst.corners.size());
    std::vector<SkinVertex> skinVertices = GetSkinVertices(dst);
    const uint32_t originBase = uint32_t(skinVertices.size());
    for (const SkinVertex& skin : GetSkinVertices(src))
        skinVertices.push_back(skin);
    SetSkinVertices(dst, skinVertices);
    for (PatchNode node : src.nodes)
    {
        for (int i = 0; i < 3; ++i)
            if (node.origin.skinVertex[i] != kNoNode)
                node.origin.skinVertex[i] += originBase;
        dst.nodes.push_back(node);
    }
    for (PatchCorner corner : src.corners)
    {
        corner.node += nodeBase;
        dst.corners.push_back(corner);
    }
    for (PatchFace face : src.faces)
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
    for (PatchNode& node : box.nodes)
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
    for (const PatchCorner& corner : result.positive.corners)
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
    PatchEdgeUsageMap usage;
    CollectEdgeUsage(piece, usage);
    uint32_t n = 0;
    for (const auto& entry : usage)
        n += entry.second.count == 1;
    return n;
}

static uint32_t CountCapFaces(const PatchPiece& piece)
{
    uint32_t n = 0;
    for (const PatchFace& face : piece.faces)
        n += face.kind == PatchFaceKind::Cap;
    return n;
}

// Dot of a cap face's geometric normal with a direction.
static double CapFacing(const PatchPiece& piece, double dx, double dy, double dz)
{
    for (const PatchFace& face : piece.faces)
    {
        if (face.kind != PatchFaceKind::Cap)
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
    CHECK(CountKind(result.positive, PatchEdgeKind::Cut) == 0 && CountKind(result.negative, PatchEdgeKind::Cut) == 0);
    CHECK(CountKind(result.positive, PatchEdgeKind::CapSeam) == 8 && CountKind(result.negative, PatchEdgeKind::CapSeam) == 8);
    CHECK(CountOpenEdges(result.positive) == 0 && CountOpenEdges(result.negative) == 0);   // closed solids
    CHECK(std::fabs(TotalArea(result.positive, PatchFaceKind::Cap) - 4.0) < 1e-5);
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
    for (const PatchFace& face : result.positive.faces)
        if (face.kind == PatchFaceKind::Cap)
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
        CHECK(std::fabs(std::fabs(TotalArea(result.positive, PatchFaceKind::Cap)) - std::fabs(expected)) < 1e-4 * std::fabs(expected));
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
    CHECK(std::fabs(TotalArea(result.positive, PatchFaceKind::Cap) - 3.0) < 1e-5);        // the L covers 3 unit squares
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
    CHECK(CountKind(second.negative, PatchEdgeKind::CapSeam) == 8 + newSeam);    // inherited old seam + new seam
    CHECK(CountKind(second.positive, PatchEdgeKind::CapSeam) == newSeam);
    CHECK(CountOpenEdges(second.negative) == 0 && CountOpenEdges(second.positive) == 0);
    for (const PatchNode& node : second.negative.nodes)
    {
        int used = 0;
        double sum = 0.0;
        for (int i = 0; i < 3; ++i)
            if (node.origin.skinVertex[i] != kNoNode)
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
    CHECK(upper.caps.size() == 1 && CountKind(upper, PatchEdgeKind::CapSeam) == 8);
}

// ---------------------------------------------------------------- debug tests (face origin)

// A surface face carries the cut that divided it; a face moved over whole keeps its old value.
// The octahedron cut through one node has (0, +, -) faces, whose opposite edge is divided.
static void CheckDividedFacesHaveCutId(const SliceResult& result, const PatchPiece& input, uint32_t cutId, const char* label)
{
    // Per source triangle: faces going in, faces coming out (both pieces). More faces out than in
    // means this cut divided one of them; otherwise every face keeps a cutId it already had.
    std::vector<uint32_t> in(input.faces.size() + 64, 0), out(in.size(), 0);
    for (const PatchFace& face : input.faces)
        if (face.kind == PatchFaceKind::Surface)
            ++in[face.sourceTriangle];
    for (const PatchPiece* piece : { &result.positive, &result.negative })
        for (const PatchFace& face : piece->faces)
            if (face.kind == PatchFaceKind::Surface)
                ++out[face.sourceTriangle];
    uint32_t divided = 0;
    for (uint32_t t = 0; t < in.size(); ++t)
    {
        if (out[t] == 0)
            continue;
        const bool wasDivided = out[t] > in[t];
        bool marked = false;
        for (const PatchPiece* piece : { &result.positive, &result.negative })
            for (const PatchFace& face : piece->faces)
            {
                if (face.kind != PatchFaceKind::Surface || face.sourceTriangle != t)
                    continue;
                marked = marked || face.cutId == cutId;
                bool known = face.cutId == cutId && wasDivided;
                for (const PatchFace& old : input.faces)
                    known = known || (old.kind == PatchFaceKind::Surface && old.sourceTriangle == t && old.cutId == face.cutId);
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
        for (const PatchFace& face : piece->faces)
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
    for (const PatchFace& face : keep.positive.faces)
        sevens += face.kind == PatchFaceKind::Surface && face.sourceTriangle == 2 && face.cutId == 7;
    CHECK(sevens == 1);
    // Cap faces keep meaning "the cut that made the cap".
    for (const PatchFace& face : second.negative.faces)
        if (face.kind == PatchFaceKind::Cap)
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
        for (const PatchFace& face : piece->faces)
            if (face.kind == PatchFaceKind::Surface)
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
    const PatchPiece box = MakeBox();
    SliceResult expected, actual;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), expected), SliceStatus::Ok, expected.detail);
    const SlicePlane converted = HorizontalPlane(0, 1, 0, 5.0f).ToSlicePlane();
    CHECK(std::fabs(converted.normal.y - 1.0f) < 1e-6f && std::fabs(converted.point.y - 1.0f) < 1e-6f);
    CHECK_STATUS(SlicePatch(box, converted, 1, CapOptions(), actual), SliceStatus::Ok, actual.detail);
    CHECK(actual.positive.faces.size() == expected.positive.faces.size() && actual.negative.faces.size() == expected.negative.faces.size());
    CHECK(std::fabs(TotalSurfaceArea(actual.positive) - TotalSurfaceArea(expected.positive)) < 1e-4);

    // Turned over, the normal points down and the + and - pieces swap.
    CutPlane over = HorizontalPlane(0, 1, 0, 5.0f);
    over.RotateAroundLocalAxis(0, XM_PI);
    const SlicePlane flipped = over.ToSlicePlane();
    CHECK(flipped.normal.y < -0.99f);
    SliceResult flippedResult;
    CHECK_STATUS(SlicePatch(box, flipped, 1, CapOptions(), flippedResult), SliceStatus::Ok, flippedResult.detail);
    CHECK(std::fabs(TotalSurfaceArea(flippedResult.positive) - TotalSurfaceArea(expected.negative)) < 1e-4);
}

static void TestLoopsInsidePlane()
{
    // The box (x, z in [-1, 1], y in [0, 2]) cut at y = 1 has a square loop with corners at +-1 and
    // collinear points in the middle of each side.
    const PatchPiece box = MakeBox();
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
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

    // No loops (nothing to check) passes; a loop that names a missing node fails.
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
        pose.parents.push_back(i == 0 ? kNoNode : i - 1);
    }
    return pose;
}

// Root (0,2,0) with two legs: hip 1 and knee 2 on the left (x = -0.5), hip 3 and knee 4 on the right (x = +0.5).
static SkeletonPose MakeLegsPose()
{
    SkeletonPose pose;
    pose.jointPositions = { { 0.0f, 2.0f, 0.0f }, { -0.5f, 1.8f, 0.0f }, { -0.5f, 1.0f, 0.0f }, { 0.5f, 1.8f, 0.0f }, { 0.5f, 1.0f, 0.0f } };
    pose.parents = { kNoNode, 0, 1, 0, 3 };
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
    pose.parents = { kNoNode, 0, 1, 1 };
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
        for (const ProceduralVertex& v : data.vertices)
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
        for (const ProceduralVertex& v : d.vertices)
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
    std::vector<SkinVertex> skinVertices = GetSkinVertices(box);
    for (size_t i = 0; i < box.nodes.size(); ++i)
    {
        SkinVertex& skin = skinVertices[i];
        skin.bindPosition = box.nodes[i].position;
        skin.jointCount = 2;
        skin.joints[0] = 0;
        skin.joints[1] = 1;
        const float w1 = box.nodes[i].position.y * 0.5f * 0.8f + 0.1f;      // 0.1 .. 0.9
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
    for (PatchNode& node : box.nodes)
        node.position = { node.position.x + 100.0f, -7.0f, 3.0f };
    SkinStats stats;
    std::string detail;
    CHECK(box.skin->Apply(box, IdentitySlots(1), &stats, &detail));
    CHECK(MaxNodeDelta(box, reference) == 0.0);
    CHECK(stats.skinVerticesEvaluated == 8 && stats.surfaceVerticesWritten == 8 && stats.capVerticesSkipped == 0);
}

// 2. One joint with a rotation and a translation: bind * R * T, row-vector convention.
static void TestSkinSingleJoint()
{
    PatchPiece tri = MakePiece({ { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, { { 0, 1, 2 } });
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
        CHECK(std::fabs(tri.nodes[i].position.x - expected[i].x) < 1e-5f && std::fabs(tri.nodes[i].position.y - expected[i].y) < 1e-5f
            && std::fabs(tri.nodes[i].position.z - expected[i].z) < 1e-5f);
}

// 3. Two joints with weights 0.25 / 0.75, computed by hand:
//    0.25 * (5,1,1) + 0.75 * (1,9,1) = (2,7,1).
static void TestSkinTwoJointBlend()
{
    PatchPiece tri = MakePiece({ { 1, 1, 1 }, { 2, 2, 2 }, { 3, 3, 3 } }, { { 0, 1, 2 } });
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
    CHECK(std::fabs(tri.nodes[0].position.x - 2.0f) < 1e-6f && std::fabs(tri.nodes[0].position.y - 7.0f) < 1e-6f
        && std::fabs(tri.nodes[0].position.z - 1.0f) < 1e-6f);
    // The same blend straight from SkinPoint.
    XMFLOAT3 point;
    DirectX::XMStoreFloat3(&point, Skinning::SkinPoint(slots[0].data(), 2, skin.joints, skin.weights, 2, skin.bindPosition));
    CHECK(point.x == tri.nodes[0].position.x && point.y == tri.nodes[0].position.y && point.z == tri.nodes[0].position.z);
}

// 4. A piece cut in a pose and skinned with the same matrices again lands on the cut positions:
//    cut nodes are driven by their PatchOrigin, with no joint or weight of their own.
static void TestSkinMatchesSliceNodes()
{
    const SlotMatrices pose = PoseA();
    const PatchPiece box = MakeSkinnedBox(pose);
    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 2.0f, 0, 0.3f, 1.0f, 0.2f), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    for (PatchPiece* piece : { &result.positive, &result.negative })
    {
        PatchPiece skinned = *piece;
        for (PatchNode& node : skinned.nodes)
            node.position = { 0.0f, 0.0f, 0.0f };
        SkinStats stats;
        std::string detail;
        CHECK(skinned.skin->Apply(skinned, pose, &stats, &detail));
        const double tolerance = 1e-5 * ComputePieceRadius(*piece);
        CHECK(MaxNodeDelta(skinned, *piece) <= tolerance);
        CHECK(stats.capVerticesSkipped == 0);
        CHECK(stats.skinVerticesEvaluated <= box.skin->GetVertices().size());
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
        for (const PatchNode& node : piece->nodes)
            CHECK(node.kind == PatchNodeKind::Surface);          // no cap-only nodes exist
        ProceduralMeshData baked;
        BuildRenderMesh(*piece, baked);
        const SlotMatrices pose = PoseA();
        SkinStats skinStats;
        CHECK(piece->skin->Apply(*piece, pose, &skinStats, &detail));
        CHECK(skinStats.capVerticesSkipped == 0);
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
        for (const PatchFace& face : piece->faces)
            capSeen = capSeen || face.kind == PatchFaceKind::Cap;
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
    CHECK(!box.skin->Apply(box, IdentitySlots(1), nullptr, &detail));
    CHECK(detail.find("skin state") != std::string::npos);
    CHECK(MaxNodeDelta(box, before) == 0.0);
}

// 7. Cutting a piece twice: the second generation skins back to its own positions as well.
static void TestSkinAfterTwoCuts()
{
    const SlotMatrices pose = PoseA();
    const PatchPiece box = MakeSkinnedBox(pose);
    double yLow = 1e30, yHigh = -1e30;
    for (const PatchNode& node : box.nodes)
    {
        yLow = std::min<double>(yLow, node.position.y);
        yHigh = std::max<double>(yHigh, node.position.y);
    }
    SliceResult first, second;
    const float yMid = float((yLow + yHigh) * 0.5);
    CHECK_STATUS(SlicePatch(box, MakePlane(0, yMid, 0, 0, 1, 0), 1, CapOptions(), first), SliceStatus::Ok, first.detail);
    // Cut the upper half again, a quarter of the way up. Only the surface is crossed.
    double upLow = 1e30, upHigh = -1e30;
    for (const PatchNode& node : first.positive.nodes)
    {
        upLow = std::min<double>(upLow, node.position.y);
        upHigh = std::max<double>(upHigh, node.position.y);
    }
    const float yQuarter = float(upLow + (upHigh - upLow) * 0.4);
    CHECK_STATUS(SlicePatch(first.positive, MakePlane(0, yQuarter, 0, 0, 1, 0), 2, CapOptions(), second), SliceStatus::Ok, second.detail);
    for (PatchPiece* piece : { &second.positive, &second.negative })
    {
        PatchPiece skinned = *piece;
        for (PatchNode& node : skinned.nodes)
            node.position = { 9.0f, 9.0f, 9.0f };
        std::string detail;
        CHECK(skinned.skin->Apply(skinned, pose, nullptr, &detail));
        CHECK(MaxNodeDelta(skinned, *piece) <= 1e-5 * ComputePieceRadius(*piece));
    }
    // 두 번째 세대의 절단 정점은 스킨 정점 최대 3개에 의존한다.
    for (const PatchNode& node : second.positive.nodes)
        CHECK(node.origin.skinVertex[0] != kNoNode);
    // New pose: the second-generation piece moves with the same matrices.
    PatchPiece moved = second.positive;
    CHECK(moved.skin->Apply(moved, PoseB()));
    CHECK(MaxNodeDelta(moved, second.positive) > 0.1);
}

// Bad input is refused without touching the piece.
// 용접이 만든 skin은 자른 두 piece가 같은 객체로 공유하고, skin이 없는 piece는 스킨 정점이 없는 표로 검증한다.
static void TestSkinningShared()
{
    PatchPiece box;
    WeldStats stats;
    std::string detail;
    CHECK_STATUS(BuildPatchPiece(MakeBoxWeldInput(), kDefaultEpsilonScale, box, stats, &detail), SliceStatus::Ok, detail);
    CHECK(box.skin != nullptr && box.skin->GetVertices().size() == 8);

    SliceResult result;
    CHECK_STATUS(SlicePatch(box, MakePlane(0, 1, 0, 0, 1, 0), 1, CapOptions(), result), SliceStatus::Ok, result.detail);
    CHECK(result.positive.skin == box.skin && result.negative.skin == box.skin);
    CHECK(box.skin.use_count() >= 3);

    // skin이 없으면 Surface 정점의 origin이 범위 밖이라 InvalidOrigin이다.
    PatchPiece noSkin = box;
    noSkin.skin.reset();
    CHECK_STATUS(ValidatePatchPiece(noSkin, kDefaultEpsilonScale, &detail), SliceStatus::InvalidOrigin, detail);
    CHECK(box.skin != nullptr);
}

static void TestSkinRefusesBadInput()
{
    PatchPiece box = MakeSkinnedBox(PoseA());
    const PatchPiece before = box;
    std::string detail;
    CHECK(!box.skin->Apply(box, IdentitySlots(1), nullptr, &detail));           // joint 1 has no matrix
    CHECK(detail.find("joint") != std::string::npos && MaxNodeDelta(box, before) == 0.0);

    CHECK(!box.skin->Apply(box, SlotMatrices(), nullptr, &detail));             // no slots at all
    CHECK(detail.find("slot") != std::string::npos && MaxNodeDelta(box, before) == 0.0);

    PatchPiece badOrigin = before;
    badOrigin.nodes[2].origin.skinVertex[0] = 99;
    CHECK(!badOrigin.skin->Apply(badOrigin, PoseA(), nullptr, &detail));
    CHECK(detail.find("skin vertex") != std::string::npos && MaxNodeDelta(badOrigin, before) == 0.0);
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

    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures;
}
