// VS Developer PowerShell, 저장소 루트: 산출물은 TEMP/SkeletalPatchTests에 둔다.
// $sp = Join-Path $env:TEMP 'SkeletalPatchTests'; New-Item -ItemType Directory -Force $sp | Out-Null
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /I MiniEngine/Core "/Fo:$sp/" "/Fe:$sp/SkeletalPatchTests.exe" MiniEngine/ModelViewer/Scene/tests/SkeletalPatchTests.cpp MiniEngine/MeshCutting/Geometry/CutPlane.cpp
// & "$sp/SkeletalPatchTests.exe"
// Release는 /O2 /DNDEBUG를 추가한다. GPU와 Jolt는 필요 없다.
#include "SkeletalPatchReference.h"
#include "../SegmentJointSelection.h"
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

// 작은 합성 입력으로 본 선택과 패치 출처의 결정성을 검증한다.
int main()
{
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
        const auto selected = Viewer::SelectSegmentJoint(parents, positions, palettes, plane);
        Check(selected.NodeId == i, "chain segment parent or terminal leaf");
        Check(selected.Kind == (i < 2 ? Viewer::SegmentJointKind::Segment : Viewer::SegmentJointKind::Leaf), "selection kind");
        Check(selected.ChildNodeId == (i < 2 ? i + 1 : UINT32_MAX), "segment child");
        Check(float(Length(selected.Intersection - Vector3(0, i < 2 ? float(i * 2) - 1 : 2, 0))) < 1e-5f, "intersection or leaf position");
        if (i == 0)
            Check(selected.PaletteMasks == std::vector<std::vector<uint32_t>>{{1, 0, 1, 0}, {0, 1}}, "duplicate palette masks per mesh");
    }

    // 비본 노드는 건너뛰며 사각형 밖 교점은 segment 후보에서 제외한다.
    Check(MeshCutting::TryBuildCutPlaneData({2, 2}, AffineTransform(kIdentity), plane), "origin plane");
    Check(Viewer::SelectSegmentJoint(parents, {Vector3(0, -2, 0), Vector3(0, -1, 0), Vector3(0, 2, 0)}, {{0, 2}}, plane).NodeId == 0, "skip non-joint ancestor");
    const auto outside = Viewer::SelectSegmentJoint({UINT32_MAX, 0}, {Vector3(3, -1, 0), Vector3(3, 1, 0)}, {{0, 1}}, plane);
    Check(outside.NodeId == 1 && outside.Kind == Viewer::SegmentJointKind::Leaf, "finite rectangle excludes crossing");

    // 두 교차 segment 중 평면 중심에 가까운 교점을 가진 부모만 고른다.
    const auto closest = Viewer::SelectSegmentJoint({UINT32_MAX, 0, UINT32_MAX, 2},
        {Vector3(0.8f, -1, 0), Vector3(0.8f, 1, 0), Vector3(0.2f, -1, 0), Vector3(0.2f, 1, 0)}, {{0, 1, 2, 3}}, plane);
    Check(closest.NodeId == 2 && closest.ChildNodeId == 3, "nearest intersection wins");

    // 별도 skeleton root는 앞선 skin joint와 segment를 만들지 않는다.
    const auto root = Viewer::SelectSegmentJoint({UINT32_MAX, UINT32_MAX, 1},
        {Vector3(0, -1, 0), Vector3(0, 1, 0), Vector3(0, 2, 0)}, {{0, 1, 2}}, plane);
    Check(root.NodeId == 0 && root.Kind == Viewer::SegmentJointKind::Leaf, "skeleton root breaks ancestry");

    // 모든 검사를 통과한 경우만 정상 종료한다.
    std::printf("SkeletalPatchTests PASS: %u checks\n", Checks);
}
