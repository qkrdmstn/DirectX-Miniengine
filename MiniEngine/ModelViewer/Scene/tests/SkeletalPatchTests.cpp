// VS Developer PowerShell, 저장소 루트: 산출물은 TEMP/SkeletalPatchTests에 둔다.
// $sp = Join-Path $env:TEMP 'SkeletalPatchTests'; New-Item -ItemType Directory -Force $sp | Out-Null
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /I MiniEngine/Core "/Fo:$sp/" "/Fe:$sp/SkeletalPatchTests.exe" MiniEngine/ModelViewer/Scene/tests/SkeletalPatchTests.cpp MiniEngine/MeshCutting/Geometry/CutPlane.cpp
// & "$sp/SkeletalPatchTests.exe"
// Release는 /O2 /DNDEBUG를 추가한다. GPU와 Jolt는 필요 없다.
#include "SkeletalPatchReference.h"
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
    if (!condition) throw std::runtime_error(message);
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
        if (r32) section.Indices32 = { 77, 1, 2, 3, 1, 2, 3 };
        else section.Indices16 = { 77, 1, 2, 3, 1, 2, 3 };
        section.Draws = { {1, 6, -1}, {4, 3, -1} };
        input.Sections = { section, section };
        auto selected = SelectJointReference(input);
        Check(selected.NodeId == 7 && selected.CrossingTriangles == 6, "mapping, normalization, duplicate palette, plane vertex");
        const auto patch = ExtractPatchReference(input, 7);
        Check(patch.size() == 6 && patch[0].Vertices == std::array<uint32_t, 3>{0, 1, 2}, "index formats and offsets");
        Check(patch[0].SectionId == 0 && patch[0].DrawId == 0 && patch[0].TriangleId == 0
            && patch[1].TriangleId == 1 && patch[2].DrawId == 1 && patch[2].TriangleId == 0
            && patch[3].SectionId == 1 && patch[5].DrawId == 1, "source output order");

        // weight 합이 다른 꼭짓점도 정규화하고 동점에는 작은 NodeId를 고른다.
        input.Sections.resize(1);
        auto& vertices = input.Sections[0].Vertices;
        vertices[0].Weights = {2, 0, 0, 0};
        vertices[1].Weights = {0, 600, 0, 0};
        vertices[2].Weights = {0, 0, 0, 0};
        Check(SelectJointReference(input).NodeId == 7, "normalized tie selects smaller NodeId");
        vertices[0].Weights = {65535, 0, 0, 0};
        vertices[1].Weights = {0, 1, 0, 0};
        Check(ExtractPatchReference(input, 7).size() == 3, "one positive corner includes whole triangle");
        Check(ExtractPatchReference(input, 99).empty() && ExtractPatchReference(input, InvalidNode).empty(), "no unrelated or invalid node");

        // eps 경계 접촉과 공면 삼각형은 양쪽 엄격 부호 조건을 만족하지 않는다.
        vertices[0].Position = Vector3(0, -input.Epsilon, 0);
        Check(SelectJointReference(input).CrossingTriangles == 0, "epsilon boundary contact");
        vertices[0].Position = Vector3(-1, 0, 0);
        vertices[1].Position = Vector3(1, 0, 0);
        Check(SelectJointReference(input).NodeId == InvalidNode, "coplanar triangle");
        vertices[0].Position = Vector3(0, -1, 4);
        vertices[1].Position = Vector3(0, 1, 4);
        vertices[2].Position = Vector3(1, 0, 4);
        Check(SelectJointReference(input).CrossingTriangles == 0, "segment outside finite rectangle");

        // 양 끝이 사각형 밖이어도 내부를 통과하면 교차로 센다.
        vertices[0].Position = Vector3(-4, -1, 0);
        vertices[1].Position = Vector3(-4, 1, 0);
        vertices[2].Position = Vector3(4, input.Epsilon * 0.5f, 0);
        Check(SelectJointReference(input).CrossingTriangles == 3, "outside endpoints and epsilon-zero projection");
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

        // 정수 반올림 후 사라지는 미소 표는 실수 합산과 다른 동점 결과를 만든다.
        input.JointNodes = {99, 20, 7, 99};
        input.Sections = {section};
        input.Sections[0].Vertices[0].Weights = {1, 0, 0, 0};
        input.Sections[0].Vertices[1].Weights = {0, 1, 0, 0};
        input.Sections[0].Vertices[2].Palette = {0, 1, 2, 2};
        input.Sections[0].Vertices[2].Weights = {1, 0, 65535, 65535};
        Check(SelectJointReference(input).NodeId == 7, "fixed-point rounding precedes accumulation");
    }

    // 모든 검사를 통과한 경우만 정상 종료한다.
    std::printf("SkeletalPatchTests PASS: %u checks\n", Checks);
}
