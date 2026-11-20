// VS Developer PowerShell, 저장소 루트: 산출물은 TEMP/SkeletalPatchTests에 둔다.
// $sp = Join-Path $env:TEMP 'SkeletalPatchTests'; New-Item -ItemType Directory -Force $sp | Out-Null
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /I MiniEngine/Core "/Fo:$sp/" "/Fe:$sp/SkeletalPatchTests.exe" MiniEngine/ModelViewer/Scene/tests/SkeletalPatchTests.cpp MiniEngine/MeshCutting/Geometry/CutPlane.cpp MiniEngine/Model/LatticeTopology.cpp
// & "$sp/SkeletalPatchTests.exe"
// Release는 /O2 /DNDEBUG를 추가한다. GPU와 Jolt는 필요 없다.
#include "SkeletalPatchReference.h"
#include "../SegmentJointSelection.h"
#include "../TriangleMaskCompaction.h"
#include <cstdio>
#include <stdexcept>
using namespace Viewer::PatchReference;
using namespace Math;
static uint32_t Checks = 0;

// Release에서도 계약 위반을 예외로 보고한다.
static void Check(bool condition, const char* message)
{
    // 성공한 검사도 세어 실행 범위를 보고한다.
    ++Checks;
    if (!condition)
        throw std::runtime_error(message);
}

// 무한 평면 교차 집합과 자손이 있는 단일 루트의 기존 선택 마스크 동등성을 검증한다.
static void CheckCrossingJoints()
{
    // 형제 P 둘의 교점은 유한 사각형 밖에 있어도 모두 선택한다.
    MeshCutting::CutPlaneData plane;
    Check(MeshCutting::TryBuildCutPlaneData({2, 2}, AffineTransform(kIdentity), plane), "multi plane");
    const std::vector<uint32_t> parents = {UINT32_MAX, 0, 0, 1, 2};
    const std::vector<Vector3> positions = {Vector3(0, -2, 0), Vector3(-4, -1, 0),
        Vector3(4, -1, 0), Vector3(-4, 1, 0), Vector3(4, 1, 0)};
    const std::vector<std::vector<uint32_t>> palettes = {{0, 1, 2, 3, 4, 1}, {2, 4}};
    const std::vector<uint8_t> influencing(5, 1);
    const auto siblings = Viewer::SelectCrossingJoints(parents, positions, palettes, plane, influencing);
    Check(siblings.size() == 2 && siblings[0].NodeId == 1 && siblings[1].NodeId == 2,
        "two sibling P roots outside finite rectangle");
    Check(siblings[0].ChildNodeId == 3 && siblings[1].ChildNodeId == 4
        && siblings[0].ChildSide == 0 && siblings[1].ChildSide == 0, "sibling snapshots and B sides");
    std::printf("MI4a siblings P={%u,%u} child={%u,%u} Bside={%u,%u}\n",
        siblings[0].NodeId, siblings[1].NodeId, siblings[0].ChildNodeId,
        siblings[1].ChildNodeId, siblings[0].ChildSide, siblings[1].ChildSide);

    // 형제 본 자체와 중복 팔레트를 합치며 자손은 명시적으로 선택할 때만 포함한다.
    const auto unionMask = Viewer::BuildJointUnionMasks(parents, palettes, {1, 2});
    Check(unionMask == std::vector<std::vector<uint32_t>>{{0, 1, 1, 0, 0, 1}, {1, 0}},
        "sibling union excludes descendants and preserves duplicate palettes");
    Check(Viewer::BuildJointUnionMasks(parents, palettes, {1, 2, 1}) == unionMask,
        "duplicate roots are idempotent");
    Check(Viewer::BuildJointUnionMasks(parents, palettes, {1})
        == std::vector<std::vector<uint32_t>>{{0, 1, 0, 0, 0, 1}, {0, 0}}, "single P node mask");
    Check(Viewer::BuildJointUnionMasks(parents, palettes, {})
        == std::vector<std::vector<uint32_t>>{{0, 0, 0, 0, 0, 0}, {0, 0}}, "empty root set");
    Check(Viewer::BuildJointUnionMasks(parents, palettes, {5}).empty(), "invalid root rejected");
    Check(Viewer::BuildJointUnionMasks({1, 0}, {{0}}, {0}).empty(), "cyclic hierarchy rejected");

    // 자손이 있는 본을 기존 선택기로 고르고 단일 루트 합집합 마스크와 직접 대조한다.
    MeshCutting::CutPlaneData legacyPlane;
    Check(MeshCutting::TryBuildCutPlaneData({100, 100}, AffineTransform(kIdentity), legacyPlane), "legacy plane");
    const auto legacySelection = Viewer::SelectSegmentJoint(parents, positions, palettes, legacyPlane, influencing);
    Check(legacySelection.NodeId == 1 && parents[3] == legacySelection.NodeId,
        "legacy single selection has descendants");
    const bool equivalent = Viewer::BuildJointUnionMasks(parents, palettes, {legacySelection.NodeId})
        == legacySelection.PaletteMasks;
    std::printf("MI4b n=1 legacy mask with descendants equivalent=%s\n", equivalent ? "PASS" : "FAIL");
    std::fflush(stdout);
    Check(equivalent, "n=1 union mask equals legacy single-selection mask with descendants");

    // 두 P가 같은 삼각형에 영향을 주어도 한 번만 추출하고 제거 상태를 적용한다.
    Input unionInput;
    unionInput.JointNodes = {1, 2, 0};
    Section section;
    section.JointCount = 3;
    section.Vertices = {{Vector3(kZero), {0, 1, 2, 0}, {1, 65534, 0, 0}},
        {Vector3(kZero), {2, 0, 0, 0}, {65535, 0, 0, 0}},
        {Vector3(kZero), {1, 0, 0, 0}, {65535, 0, 0, 0}}};
    section.Indices16 = {0, 1, 2, 1, 1, 1};
    section.Draws = {{0, 6, 0}};
    unionInput.Sections.push_back(section);
    const auto combined = ExtractPatchReference(unionInput, InvalidNode, {{1, 1, 0}});
    Check(combined.size() == 1 && combined[0].TriangleId == 0, "overlapping triangle emitted once");
    const auto single = ExtractPatchReference(unionInput, 1);
    const auto singleMask = ExtractPatchReference(unionInput, InvalidNode, {{1, 0, 0}});
    Check(single.size() == singleMask.size() && single[0].TriangleId == singleMask[0].TriangleId,
        "n=1 mask equals existing positive weight selection");
    unionInput.State = {0x80000000u, 0};
    Check(ExtractPatchReference(unionInput, InvalidNode, {{1, 1, 0}}).empty(), "union excludes removed triangle");

    // 부호 반전은 P와 교점을 보존하고 B 반공간만 뒤집는다.
    plane.WorldPlane *= -1.0f;
    const auto flipped = Viewer::SelectCrossingJoints(parents, positions, palettes, plane, influencing);
    Check(flipped.size() == siblings.size(), "sign flip count");
    for (size_t i = 0; i < siblings.size(); ++i)
    {
        Check(flipped[i].NodeId == siblings[i].NodeId && flipped[i].ChildNodeId == siblings[i].ChildNodeId
            && float(Length(flipped[i].Intersection - siblings[i].Intersection)) < 1e-6f
            && flipped[i].ChildSide == 1 - siblings[i].ChildSide, "sign flip preserves selection and reverses B");
    }
    std::printf("MI4a signFlip P={%u,%u} Bside={%u,%u}\n",
        flipped[0].NodeId, flipped[1].NodeId, flipped[0].ChildSide, flipped[1].ChildSide);
    plane.WorldPlane *= -1.0f;

    // 번호상 뒤에 있는 조상도 먼저 교차한 자손 후보를 제거한다.
    const auto ancestor = Viewer::SelectCrossingJoints({2, 0, UINT32_MAX},
        {Vector3(0, 1, 0), Vector3(0, -1, 0), Vector3(0, -1, 0)}, {{0, 1, 2}}, plane, {1, 1, 1});
    Check(ancestor.size() == 1 && ancestor[0].NodeId == 2 && ancestor[0].ChildNodeId == 0,
        "crossing ancestor removes descendant independent of node order");
    std::printf("MI4a ancestor+descendant P={%u} child=%u\n", ancestor[0].NodeId, ancestor[0].ChildNodeId);

    // 비영향 팔레트 본과 영향이 있는 비팔레트 노드를 함께 건너뛴다.
    const auto skipped = Viewer::SelectCrossingJoints({UINT32_MAX, 0, 1, 2},
        {Vector3(0, -1, 0), Vector3(0, -1, 0), Vector3(0, -1, 0), Vector3(0, 1, 0)},
        {{0, 1, 3}}, plane, {1, 0, 1, 1});
    Check(skipped.size() == 1 && skipped[0].NodeId == 0 && skipped[0].ChildNodeId == 3,
        "skip uninfluencing and non-palette intermediates");
    Check(Viewer::IsSelectedSegmentChild(0, 3, 2, skipped[0].ChildNodeId), "indirect child snapshot reusable");
    std::printf("MI4a skipped P=%u child=%u Bside=%u\n", skipped[0].NodeId,
        skipped[0].ChildNodeId, skipped[0].ChildSide);

    // 같은 P는 가장 가까운 자식 하나만 보존하며 거리 동점은 작은 자식 번호다.
    const auto branch = Viewer::SelectCrossingJoints({UINT32_MAX, 0, 0, 0},
        {Vector3(0, -1, 0), Vector3(4, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0)},
        {{3, 2, 1, 0}}, plane, {1, 1, 1, 1});
    Check(branch.size() == 1 && branch[0].ChildNodeId == 2, "same P nearest child and stable tie");

    // 허용오차 끝점은 격자 교차 함수와 같은 방향을 쓰고 공면과 미교차는 제외한다.
    for (bool flip : {false, true})
    {
        const auto endpoint = Viewer::SelectCrossingJoints({UINT32_MAX, 0},
            {Vector3(0, -1, 0), Vector3(0, -1e-5f, 0)}, {{0, 1}}, plane, {1, 1});
        Check(endpoint.size() == 1 && endpoint[0].ChildSide == (flip ? 1u : 0u)
            && float(Length(endpoint[0].Intersection - Vector3(0, -1e-5f, 0))) < 1e-7f, "epsilon endpoint direction");
        plane.WorldPlane *= -1.0f;
    }
    Check(Viewer::SelectCrossingJoints({UINT32_MAX, 0}, {Vector3(kZero), Vector3(1, 0, 0)},
        {{0, 1}}, plane, {1, 1}).empty(), "coplanar has no unique crossing");
    Check(Viewer::SelectCrossingJoints({UINT32_MAX, 0}, {Vector3(0, -2, 0), Vector3(0, -1, 0)},
        {{0, 1}}, plane, {1, 1}).empty(), "no crossing has no leaf fallback");
}

// 형제 그룹의 고정 팔레트와 LCA 부착 및 단일 본의 기존 규칙 동등성을 검증한다.
static void CheckCutBodyGroups()
{
    // 중복과 순서가 섞인 팔레트도 노드 그룹을 정확히 참조한다.
    const std::vector<uint32_t> parents = {UINT32_MAX, 0, 0, 1, 2, 0};
    const std::vector<uint32_t> joints = {4, 0, 3, 2, 1, 3, 5};
    const auto siblings = Viewer::BuildCutBodyGroups(parents, joints, {1, 2});
    Check(siblings.NodeGroups == std::vector<uint32_t>({0, 1, 2, 1, 2, 0}), "sibling groups");
    Check(siblings.AttachNodes == std::vector<uint32_t>({0, 1, 2}), "sibling attachments");
    Check(siblings.FrozenPalettes == std::vector<std::vector<uint8_t>>({
        {1, 0, 1, 1, 1, 1, 0}, {1, 1, 0, 1, 0, 0, 1}, {0, 1, 1, 0, 1, 1, 1}}),
        "A/B1/B2 Frozen palettes");
    std::printf("MI4c siblings groups=0,1,2,1,2,0 attach=0,1,2 Frozen A/B1/B2 PASS\n");

    // n=1은 기존 자손 판정과 A 부모 및 B 루트 부착을 그대로 보존한다.
    const auto single = Viewer::BuildCutBodyGroups(parents, joints, {1});
    Check(single.AttachNodes == std::vector<uint32_t>({0, 1}), "n=1 attachments");
    for (size_t k = 0; k < joints.size(); ++k)
    {
        bool descendant = false;
        for (uint32_t node = joints[k]; node != UINT32_MAX; node = parents[node])
            descendant |= node == 1;
        Check((single.FrozenPalettes[0][k] != 0) == descendant, "n=1 Frozen A equivalent");
        Check((single.FrozenPalettes[1][k] != 0) == !descendant, "n=1 Frozen B equivalent");
    }

    // 부모가 서로 다른 분기도 조상 번호 순서와 무관하게 최소 공통 조상을 찾는다.
    const auto cousins = Viewer::BuildCutBodyGroups({2, 2, UINT32_MAX, 0, 1}, {}, {3, 4});
    Check(cousins.AttachNodes == std::vector<uint32_t>({2, 3, 4}), "parent LCA across branches");
    Check(Viewer::BuildCutBodyGroups(parents, joints, {1, 3}).NodeGroups.empty(), "nested roots rejected");
    Check(Viewer::BuildCutBodyGroups(parents, joints, {1, 1}).NodeGroups.empty(), "duplicate roots rejected");
    Check(Viewer::BuildCutBodyGroups({1, 0}, {}, {1}).NodeGroups.empty(), "cycle rejected");
    Check(Viewer::BuildCutBodyGroups(parents, {6}, {1}).NodeGroups.empty(), "bad palette rejected");
}

// D1의 유한 선택과 문서 사례 5~9를 물리 및 GPU 없이 검증한다.
static void CheckRigCutJoints()
{
    // 사례 5는 가까운 owner를 고르고 동거리에서는 작은 owner ID를 고른다.
    using namespace Viewer;
    MeshCutting::CutPlaneData plane;
    Check(MeshCutting::TryBuildCutPlaneData({0.3f, 0.3f}, AffineTransform(kIdentity), plane), "rig plane");
    const std::vector<std::vector<uint32_t>> parents = {{UINT32_MAX, 0, 1, 0, 3}, {UINT32_MAX, 0, 1}};
    std::vector<RigCutSegment> segments = {
        {10, 0, 1, 2, Vector3(0.08f, -0.1f, 0), Vector3(0.08f, 0.1f, 0), true},
        {20, 1, 1, 2, Vector3(0.02f, -0.1f, 0), Vector3(0.02f, 0.1f, 0), true}};
    auto selected = SelectRigCutJoints(segments, parents, plane);
    Check(selected.Result == RigCutResult::Selected && selected.OwnerId == 20 && selected.OwnerIndex == 1
        && selected.Joints.size() == 1 && selected.Joints[0].ParentNode == 1, "case 5 nearest owner");
    segments[0].Parent = segments[1].Parent;
    segments[0].Child = segments[1].Child;
    std::swap(segments[0], segments[1]);
    Check(SelectRigCutJoints(segments, parents, plane).OwnerId == 10, "case 5 tie smaller owner ID");

    // 사례 6은 AnimationFrame 끝점을 쓰고 non-live 교차만 있으면 거절한다.
    segments = {{10, 0, 1, 2, Vector3(0, -0.1f, 0), Vector3(0, 0.1f, 0), true}};
    selected = SelectRigCutJoints(segments, parents, plane);
    Check(selected.Result == RigCutResult::Selected && selected.Joints.size() == 1
        && selected.Joints[0].ChildNode == 2 && selected.Joints[0].ChildSide == 0
        && float(Length(selected.Joints[0].Intersection)) < 1e-6f, "case 6a live AnimationFrame child");
    segments[0].Live = false;
    selected = SelectRigCutJoints(segments, parents, plane);
    Check(selected.Result == RigCutResult::AlreadyBroken && selected.Joints.empty(), "case 6b unavailable crossing");

    // 사례 7의 root는 선택하며 사례 8의 같은 P 동점은 입력상 첫 자식을 보존한다.
    segments = {{10, 0, 0, 1, Vector3(0, -0.1f, 0), Vector3(0, 0.1f, 0), true}};
    selected = SelectRigCutJoints(segments, parents, plane);
    Check(selected.Result == RigCutResult::Selected && selected.Joints.size() == 1
        && selected.Joints[0].ParentNode == 0, "case 7 root before Request rejection");
    segments = {{10, 0, 1, 3, Vector3(0, -0.1f, 0), Vector3(0.04f, 0.1f, 0), true},
        {10, 0, 1, 2, Vector3(0, -0.1f, 0), Vector3(-0.04f, 0.1f, 0), true}};
    selected = SelectRigCutJoints(segments, parents, plane);
    Check(selected.Joints.size() == 1 && selected.Joints[0].ChildNode == 3, "case 8 first Asset.Joints child");

    // 사례 9는 반대 ChildSide를 거절하되 정렬된 snapshot을 남긴다.
    segments = {{10, 0, 3, 4, Vector3(0.06f, 0.1f, 0), Vector3(0.06f, -0.1f, 0), true},
        {10, 0, 1, 2, Vector3(0, -0.1f, 0), Vector3(0, 0.1f, 0), true},
        {10, 0, 0, 3, Vector3(1, -0.2f, 0), Vector3(0.06f, 0.1f, 0), true}};
    selected = SelectRigCutJoints(segments, parents, plane);
    Check(selected.Result == RigCutResult::ChildSideMismatch && selected.Joints.size() == 2
        && selected.Joints[0].ParentNode == 1 && selected.Joints[1].ParentNode == 3
        && selected.Joints[0].ChildSide == 0 && selected.Joints[1].ChildSide == 1, "case 9 mismatch snapshots sorted");

    // 미교차와 사각형 밖 교차 및 정확히 평면 위인 끝점은 후보가 아니다.
    Check(SelectRigCutJoints({}, parents, plane).Result == RigCutResult::NoCrossing, "rig no crossing");
    Check(SelectRigCutJoints({segments[2]}, parents, plane).Result == RigCutResult::NoCrossing, "rig outside rectangle");
    segments.resize(1);
    segments[0].Parent = Vector3(kZero);
    Check(SelectRigCutJoints(segments, parents, plane).Result == RigCutResult::NoCrossing, "rig strict zero endpoint");

    // 조상과 자손이 모두 교차하면 조상만 남기며 작은 거리에도 epsilon을 적용하지 않는다.
    segments = {{10, 0, 1, 2, Vector3(0, -1e-7f, 0), Vector3(0, 1e-7f, 0), true},
        {10, 0, 0, 1, Vector3(0.1f, -0.1f, 0), Vector3(0.1f, 0.1f, 0), true}};
    Check(SelectRigCutJoints({segments[0]}, parents, plane).Result == RigCutResult::Selected, "rig no crossing epsilon");
    selected = SelectRigCutJoints(segments, parents, plane);
    Check(selected.Result == RigCutResult::Selected && selected.Joints.size() == 1
        && selected.Joints[0].ParentNode == 0, "rig ancestor removes descendant");
}

// 패치 추출과 선택 및 몸체 그룹의 CPU 계약을 함께 검사한다.
// 원본 R16/R32의 비영 draw 시작점·baseVertex와 압축·거절의 원자성을 검사한다.
static void CheckTriangleMaskCompaction()
{
    // loader처럼 원본 draw 구간을 펼치되 기대 압축 바이트는 아래 상수로 독립 지정한다.
    const uint16_t r16[] = {99, 99, 1, 2, 3, 4, 5, 6, 99, 7, 8, 9};
    const uint32_t r32[] = {99, 70000, 70001, 70002, 80000, 80001, 80002};
    std::vector<uint32_t> a, b;
    for (uint32_t i = 2; i < 8; ++i) a.push_back(r16[i] + 10);
    for (uint32_t i = 9; i < 12; ++i) a.push_back(r16[i] + 20);
    for (uint32_t i = 1; i < 7; ++i) b.push_back(r32[i] + 30);
    std::vector<Viewer::MaskSection> sections = {
        {true, false, &a, {{6, 10}, {3, 20}}}, {true, true, &b, {{6, 30}}}};
    Viewer::CompactMask result;
    const std::vector<uint32_t> state = {0x80000001u, 0, 0, 0, 0x80000002u};
    Check(Viewer::CompactTriangleMask(sections, state, result), "mixed mask accepted");
    const uint16_t expected16[] = {4, 5, 6, 7, 8, 9};
    const uint32_t expected32[] = {70000, 70001, 70002};
    Check(result.Bytes.size() == 24 && std::memcmp(result.Bytes.data(), expected16, 12) == 0
        && std::memcmp(result.Bytes.data() + 12, expected32, 12) == 0, "independent R16/R32 bytes");
    Check(result.Sections.size() == 2 && result.Sections[0].ByteOffset == 0
        && result.Sections[0].ByteCount == 12 && result.Sections[1].ByteOffset == 12
        && result.Sections[1].ByteCount == 12, "section order and original strides");
    Check(result.Sections[0].Draws == std::vector<std::pair<uint32_t, uint32_t>>{{0, 3}, {3, 3}}
        && result.Sections[1].Draws == std::vector<std::pair<uint32_t, uint32_t>>{{0, 3}}, "draw order and ranges");

    // 뒤 section에서 거절되어도 직전의 정상 결과 전체를 보존한다.
    const auto saved = result;
    for (uint32_t invalid = 0; invalid < 3; ++invalid)
    {
        sections[1].Indices = invalid == 0 ? nullptr : &b;
        sections[1].Skinned = invalid != 1;
        if (invalid == 2) b.pop_back();
        Check(!Viewer::CompactTriangleMask(sections, state, result), "unavailable CPU section rejected");
        Check(result.Bytes == saved.Bytes && result.Sections.size() == saved.Sections.size()
            && result.Sections[0].Draws == saved.Sections[0].Draws
            && result.Sections[1].Draws == saved.Sections[1].Draws
            && result.Sections[1].ByteOffset == saved.Sections[1].ByteOffset
            && result.Sections[1].ByteCount == saved.Sections[1].ByteCount
            && result.Sections[1].Override, "rejection preserves previous output");
    }

    // 제거가 없으면 CPU 인덱스 없는 비skin section도 원본 view를 그대로 쓴다.
    sections[1] = {false, true, nullptr, {{6, 30}}};
    Check(Viewer::CompactTriangleMask(sections, std::vector<uint32_t>(5), result)
        && result.Bytes.empty() && !result.Sections[0].Override && !result.Sections[1].Override,
        "no removal needs no override");

    // 전체 제거 시에도 draw 수를 유지하고 모든 draw의 인덱스 수는 0이다.
    b.push_back(80032);
    sections[1] = {true, true, &b, {{6, 30}}};
    Check(Viewer::CompactTriangleMask(sections, std::vector<uint32_t>(5, 0x80000000u), result)
        && result.Bytes.empty() && result.Sections[0].Override && result.Sections[1].Override
        && result.Sections[0].Draws == std::vector<std::pair<uint32_t, uint32_t>>{{0, 0}, {0, 0}}
        && result.Sections[1].Draws == std::vector<std::pair<uint32_t, uint32_t>>{{0, 0}}, "all removed keeps draws");

    // R16의 홀수 삼각형 뒤 R32 시작은 DWORD 정렬을 유지한다.
    Check(Viewer::CompactTriangleMask(sections, {0, 0x80000000u, 0x80000000u, 0, 0x80000000u}, result)
        && result.Sections[0].ByteCount == 6 && result.Sections[1].ByteOffset == 8
        && result.Bytes.size() == 20, "mixed section alignment");
}

// GPU 없이 패치 참조 계산과 압축 IB의 합성 사례를 실행한다.
int main()
{
    // 압축 IB의 원본 형식·순서·실패 보존 계약을 검사한다.
    CheckTriangleMaskCompaction();

    // 다중 본 순수 함수의 교차 집합 계약을 먼저 검사한다.
    CheckRigCutJoints();
    CheckCrossingJoints();
    CheckCutBodyGroups();

    // 중복 팔레트·동점·서로 다른 mesh 시작 위치를 GPU 정수 규칙으로 검사한다.
    {
        Input input;
        Check(MeshCutting::TryBuildCutPlaneData({2, 2}, AffineTransform(kIdentity), input.Plane), "influence plane");
        input.JointNodes = {99, 20, 7, 7, 3};
        Section section;
        section.StartJoint = 1;
        section.Vertices = { {Vector3(-1, -1, 0), {0, 1, 2, 0}, {4, 3, 3, 0}},
            {Vector3(1, 1, 0), {0, 1, 2, 0}, {4, 3, 3, 0}},
            {Vector3(0, 0, 1), {0, 1, 2, 0}, {4, 3, 3, 0}} };
        section.Indices16 = {0, 1, 2};
        section.Draws = {{0, 3, 0}};
        input.Sections = {section};
        Check(SelectInfluenceJointReference(input) == 7, "duplicate palette sums by NodeId");
        for (auto& vertex : input.Sections[0].Vertices) vertex.Weights = {1, 1, 0, 0};
        Check(SelectInfluenceJointReference(input) == 7, "tie chooses smaller NodeId");

        // 뒤 section의 팔레트를 잘못 참조하지 않고 mesh의 팔레트 길이를 지킨다.
        input.Sections[0].JointCount = 1;
        Check(SelectInfluenceJointReference(input) == 20, "palette stays within mesh joint count");
        input.Sections[0].JointCount = UINT32_MAX;

        // 최대 UNORM 입력의 반올림과 32비트를 넘는 누적값을 검사한다.
        for (auto& vertex : input.Sections[0].Vertices) vertex.Weights = {65535, 65535, 65535, 65535};
        input.Sections[0].Draws.resize(50000, {0, 3, 0});
        Section competitor = section;
        competitor.StartJoint = 4;
        for (auto& vertex : competitor.Vertices) vertex.Weights = {65535, 0, 0, 0};
        competitor.Draws.resize(20000, {0, 3, 0});
        input.Sections.push_back(competitor);
        Check(SelectInfluenceJointReference(input) == 7, "uint64 votes with maximum normalization numerator");
        input.Sections.resize(1);
        input.Sections[0].Draws.resize(1);
        section.StartJoint = 4;
        for (auto& vertex : section.Vertices) vertex.Weights = {65535, 0, 0, 0};
        input.Sections.push_back(section);
        Check(SelectInfluenceJointReference(input) == 3, "multi mesh startJoint mapping");
        input.Sections.resize(1);
        input.Sections[0].Vertices[0].Position = Vector3(-1, -input.Epsilon, 0);
        Check(SelectInfluenceJointReference(input) == InvalidNode, "eps contact has no crossing votes");
        input.Sections[0].Vertices[0].Position = Vector3(-1, -1, 0);
        input.Sections[0].Vertices[2].Position = Vector3(0, input.Epsilon * 0.5f, 1);
        Check(SelectInfluenceJointReference(input) == 7, "eps projected corner votes");
        for (auto& vertex : input.Sections[0].Vertices) vertex.Position += Vector3(0, 0, 10);
        Check(SelectInfluenceJointReference(input) == InvalidNode, "no finite rectangle crossing");
    }

    // 두 인덱스 형식에서 draw offset과 음수 baseVertex까지 동일하게 검사한다.
    for (bool r32 : { false, true })
    {
        Input input;
        Check(MeshCutting::TryBuildCutPlaneData({2, 2}, AffineTransform(kIdentity), input.Plane), "plane");
        input.JointNodes = { 99, 20, 7, 7 };
        Section section;
        section.StartJoint = 1;
        section.Vertices = { { Vector3(-2, -1, 0), {0, 1, 2, 0}, {4, 3, 3, 0} },
            { Vector3(2, 1, 0), {0, 1, 2, 0}, {4, 3, 3, 0} },
            { Vector3(0, 0, 1), {0, 1, 2, 0}, {4, 3, 3, 0} } };
        if (r32)
            section.Indices32 = { 77, 1, 2, 3, 1, 2, 3 };
        else
            section.Indices16 = { 77, 1, 2, 3, 1, 2, 3 };
        section.Draws = { {1, 6, -1}, {4, 3, -1} };
        input.Sections = { section, section };
        const auto patch = ExtractPatchReference(input, 7);
        Check(patch.size() == 6 && patch[0].Vertices == std::array<uint32_t, 3>{0, 1, 2}, "index formats and offsets");
        Check(patch[0].SectionId == 0 && patch[0].DrawId == 0 && patch[0].TriangleId == 0
            && patch[1].TriangleId == 1 && patch[2].DrawId == 1 && patch[2].TriangleId == 0
            && patch[3].SectionId == 1 && patch[5].DrawId == 1, "source output order");

        // 전역 번호는 draw와 section을 넘어 누적하며 소유 id만으로는 제거하지 않는다.
        input.State = { 0x80000001u, 0, 2, 0x80000002u, 0, 0x8000ffffu };
        const auto masked = ExtractPatchReference(input, 7);
        Check(masked.size() == 3 && masked[0].TriangleId == 1 && masked[1].DrawId == 1
            && masked[2].SectionId == 1 && masked[2].TriangleId == 1, "global removed mask and owner id");
        input.State.assign(6, 0x80000001u);
        Check(ExtractPatchReference(input, 7).empty(), "all removed produces empty patch");
        input.State.assign(6, 0);
        Check(ExtractPatchReference(input, 7).size() == patch.size(), "zero mask preserves selection");
        input.State.clear();

        // 양수 weight 하나만으로도 삼각형을 포함하고 무관한 본은 제외한다.
        input.Sections.resize(1);
        auto& vertices = input.Sections[0].Vertices;
        vertices[2].Weights = {0, 0, 0, 0};
        vertices[0].Weights = {65535, 0, 0, 0};
        vertices[1].Weights = {0, 1, 0, 0};
        Check(ExtractPatchReference(input, 7).size() == 3, "one positive corner includes whole triangle");
        Check(ExtractPatchReference(input, 99).empty() && ExtractPatchReference(input, InvalidNode).empty(), "no unrelated or invalid node");

        // 투표 제거 뒤에도 추출 기준의 eps·공면·유한 사각형 교차 검사를 유지한다.
        const std::array<uint32_t, 3> indices = {0, 1, 2};
        vertices[0].Position = Vector3(0, -input.Epsilon, 0);
        Check(!CrossesRectangle(input, input.Sections[0], indices), "epsilon boundary contact");
        vertices[0].Position = Vector3(-1, 0, 0);
        vertices[1].Position = Vector3(1, 0, 0);
        Check(!CrossesRectangle(input, input.Sections[0], indices), "coplanar triangle");
        vertices[0].Position = Vector3(0, -1, 4);
        vertices[1].Position = Vector3(0, 1, 4);
        vertices[2].Position = Vector3(1, 0, 4);
        Check(!CrossesRectangle(input, input.Sections[0], indices), "crossing outside finite rectangle");
        vertices[0].Position = Vector3(-4, -1, 0);
        vertices[1].Position = Vector3(-4, 1, 0);
        vertices[2].Position = Vector3(4, input.Epsilon * 0.5f, 0);
        Check(CrossesRectangle(input, input.Sections[0], indices), "outside endpoints and epsilon-zero projection");

        // 비균일 월드 변환 뒤 길이가 달라진 평면 법선으로도 정확히 투영한다.
        input.Plane.WorldPlane *= 2.0f;
        vertices[2].Position = Vector3(4, input.Epsilon * 0.4f, 0);
        Check(CrossesRectangle(input, input.Sections[0], indices), "non-unit local plane projection");

        // 양수 baseVertex 구간의 원본 꼭짓점 번호도 보존한다.
        input.Sections[0].Draws = { {1, 3, 0} };
        input.Sections[0].Vertices.insert(input.Sections[0].Vertices.begin(), section.Vertices[0]);
        Check(ExtractPatchReference(input, 7)[0].Vertices[0] == 1, "positive index window");

        // 작은 양수도 추출하되 0인 삼각형은 제외하고 남은 원본 번호를 유지한다.
        input.Sections[0].Vertices[1].Weights = {65535, 0, 0, 0};
        input.Sections[0].Vertices[2].Weights = {65535, 0, 0, 0};
        input.Sections[0].Vertices[3].Weights = {65535, 0, 0, 0};
        input.Sections[0].Vertices.push_back({ Vector3(0, 0, 0), {0, 1, 0, 0}, {65535, 1, 0, 0} });
        input.Sections[0].Indices16 = {1, 2, 3, 1, 2, 4};
        input.Sections[0].Indices32 = r32 ? std::vector<uint32_t>{1, 2, 3, 1, 2, 4} : std::vector<uint32_t>{};
        input.Sections[0].Draws = { {0, 6, 0} };
        const auto sparsePatch = ExtractPatchReference(input, 7);
        Check(sparsePatch.size() == 1 && sparsePatch[0].TriangleId == 1
            && sparsePatch[0].Vertices[2] == 4, "zero excluded, tiny positive included, sparse triangle ID");
    }

    // 세 본 사슬의 두 segment, 말단 fallback, 중복 팔레트 마스크를 검증한다.
    const std::vector<uint32_t> parents = { UINT32_MAX, 0, 1 };
    const std::vector<Vector3> positions = { Vector3(0, -2, 0), Vector3(0, 0, 0), Vector3(0, 2, 0) };
    const std::vector<std::vector<uint32_t>> palettes = { {0, 1, 0, 2}, {2, 0} };
    MeshCutting::CutPlaneData plane;
    for (uint32_t i = 0; i < 3; ++i)
    {
        Check(MeshCutting::TryBuildCutPlaneData({2, 2}, AffineTransform(Matrix3(kIdentity), Vector3(0, float(i * 2) - 1, 0)), plane), "segment plane");
        const auto selected = Viewer::SelectSegmentJoint(parents, positions, palettes, plane, std::vector<uint8_t>(3, 1));
        Check(selected.NodeId == i, "chain segment parent or terminal leaf");
        Check(selected.Kind == (i < 2 ? Viewer::SegmentJointKind::Segment : Viewer::SegmentJointKind::Leaf), "selection kind");
        Check(selected.ChildNodeId == (i < 2 ? i + 1 : UINT32_MAX), "segment child");
        Check(float(Length(selected.Intersection - Vector3(0, i < 2 ? float(i * 2) - 1 : 2, 0))) < 1e-5f, "intersection or leaf position");
        if (i == 0)
            Check(selected.PaletteMasks == std::vector<std::vector<uint32_t>>{{1, 0, 1, 0}, {0, 1}}, "duplicate palette masks per mesh");
    }

    // 비본 노드는 건너뛰며 사각형 밖 교점은 segment 후보에서 제외한다.
    Check(MeshCutting::TryBuildCutPlaneData({2, 2}, AffineTransform(kIdentity), plane), "origin plane");
    const auto sparse = Viewer::SelectSegmentJoint(parents,
        {Vector3(0, -2, 0), Vector3(0, -1, 0), Vector3(0, 2, 0)}, {{0, 2}}, plane, std::vector<uint8_t>(3, 1));
    Check(sparse.NodeId == 0 && sparse.ChildNodeId == 2, "skip non-joint ancestor");

    // 선택된 0→2 선분은 직접 부모가 달라도 유지하고 중간 자식은 제외한다.
    Check(Viewer::IsSelectedSegmentChild(sparse.NodeId, 2, parents[2], sparse.ChildNodeId), "snapshot keeps indirect child");
    Check(!Viewer::IsSelectedSegmentChild(sparse.NodeId, 1, parents[1], sparse.ChildNodeId), "snapshot excludes other child");

    // snapshot이 없을 때는 직접 자식만 교차 후보로 허용한다.
    Check(Viewer::IsSelectedSegmentChild(0, 1, parents[1], UINT32_MAX), "fallback keeps direct child");
    Check(!Viewer::IsSelectedSegmentChild(0, 2, parents[2], UINT32_MAX), "fallback excludes indirect child");

    // 유한 사각형 밖의 교차는 말단 선택으로 돌아간다.
    const auto outside = Viewer::SelectSegmentJoint({UINT32_MAX, 0}, {Vector3(3, -1, 0), Vector3(3, 1, 0)}, {{0, 1}}, plane, std::vector<uint8_t>(2, 1));
    Check(outside.NodeId == 1 && outside.Kind == Viewer::SegmentJointKind::Leaf, "finite rectangle excludes crossing");

    // 말단 평면의 월드 반올림 오차를 허용하되 실제로 내부를 지나는 segment는 유지한다.
    const auto terminal = Viewer::SelectSegmentJoint({UINT32_MAX, 0},
        {Vector3(0, -1, 0), Vector3(0, 5e-7f, 0)}, {{0, 1}}, plane, std::vector<uint8_t>(2, 1));
    Check(terminal.NodeId == 1 && terminal.Kind == Viewer::SegmentJointKind::Leaf, "terminal world epsilon");
    const auto interior = Viewer::SelectSegmentJoint({UINT32_MAX, 0},
        {Vector3(0, -1, 0), Vector3(0, 1e-4f, 0)}, {{0, 1}}, plane, std::vector<uint8_t>(2, 1));
    Check(interior.NodeId == 0 && interior.Kind == Viewer::SegmentJointKind::Segment, "interior beyond world epsilon");

    // 두 교차 segment 중 평면 중심에 가까운 교점을 가진 부모만 고른다.
    const auto closest = Viewer::SelectSegmentJoint({UINT32_MAX, 0, UINT32_MAX, 2},
        {Vector3(0.8f, -1, 0), Vector3(0.8f, 1, 0), Vector3(0.2f, -1, 0), Vector3(0.2f, 1, 0)}, {{0, 1, 2, 3}}, plane, std::vector<uint8_t>(4, 1));
    Check(closest.NodeId == 2 && closest.ChildNodeId == 3, "nearest intersection wins");

    // 별도 skeleton root는 앞선 skin joint와 segment를 만들지 않는다.
    const auto root = Viewer::SelectSegmentJoint({UINT32_MAX, UINT32_MAX, 1},
        {Vector3(0, -1, 0), Vector3(0, 1, 0), Vector3(0, 2, 0)}, {{0, 1, 2}}, plane, std::vector<uint8_t>(3, 1));
    Check(root.NodeId == 0 && root.Kind == Viewer::SegmentJointKind::Leaf, "skeleton root breaks ancestry");

    // 영향 없는 End 본은 말단이 아니며 중간의 무영향 본도 segment 조상에서 건너뛴다.
    const auto end = Viewer::SelectSegmentJoint(parents, positions, palettes, plane, {1, 1, 0});
    Check(end.NodeId == 1 && end.Kind == Viewer::SegmentJointKind::Leaf, "uninfluencing End excluded from leaf and segment");
    const auto skipped = Viewer::SelectSegmentJoint(parents, positions, palettes, plane, {1, 0, 1});
    Check(skipped.NodeId == 0 && skipped.ChildNodeId == 2, "nearest influencing ancestor across uninfluencing joint");
    Check(Viewer::SelectSegmentJoint(parents, positions, palettes, plane, {0, 0, 0}).NodeId == UINT32_MAX,
        "no influencing joints rejects selection");

    // 모든 검사를 통과한 경우만 정상 종료한다.
    std::printf("SkeletalPatchTests PASS: %u checks\n", Checks);
}
