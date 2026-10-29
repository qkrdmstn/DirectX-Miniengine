// Worktree 루트의 VS Developer PowerShell에서 빌드하고 실행한다.
// $bcTestDir = Join-Path $env:TEMP 'BreakConstraintTests'
// New-Item -ItemType Directory -Force -Path $bcTestDir | Out-Null
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /I MiniEngine/Core /I MiniEngine/Model "/Fo:$bcTestDir/SkeletonPoseTests.obj" "/Fe:$bcTestDir/SkeletonPoseTests.exe" MiniEngine/Model/tests/SkeletonPoseTests.cpp
// & "$bcTestDir/SkeletonPoseTests.exe"

#include "SkeletonPose.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

// 모든 행렬 성분을 정확히 비교하여 무변경과 기존 평가기 동등성을 검사한다.
void CheckMatrix(const Math::Matrix4& actual, const Math::Matrix4& expected)
{
    // SIMD 행렬을 저장한 뒤 각 성분의 값이 동일한지 확인한다.
    DirectX::XMFLOAT4X4 a;
    DirectX::XMFLOAT4X4 b;
    DirectX::XMStoreFloat4x4(&a, actual);
    DirectX::XMStoreFloat4x4(&b, expected);
    for (unsigned row = 0; row < 4; ++row)
    {
        // 허용 오차 없이 출력 보존과 연산 순서를 확인한다.
        for (unsigned col = 0; col < 4; ++col)
        {
            // 실패 원인을 출력하고 즉시 종료한다.
            if (a.m[row][col] != b.m[row][col])
            {
                // 기존 테스트와 같은 실패 종료 코드를 사용한다.
                std::printf("FAIL: exact matrix equality\n");
                std::exit(1);
            }
        }
    }
}

// 조건이 거짓이면 실패 원인을 출력하고 테스트를 종료한다.
void Check(bool valid, const char* reason)
{
    // assert 비활성화 여부와 관계없이 실패를 보고한다.
    if (!valid)
    {
        std::printf("FAIL: %s\n", reason);
        std::exit(1);
    }
}

// 테스트용 노드에 식별자와 DFS 플래그 및 고유한 로컬 변환을 넣는다.
GraphNode Node(uint32_t id, bool children = false, bool sibling = false, bool root = false)
{
    // 행렬 인덱스 비트 폭을 지키고 나머지 필드는 초기화한다.
    GraphNode node{};
    node.matrixIdx = id & 0x0fffffff;
    node.hasChildren = children;
    node.hasSibling = sibling;
    node.skeletonRoot = root;
    node.xform = Math::Matrix4(DirectX::XMMatrixTranslation(float(id + 1), float(id * 2), 1.0f))
        * Math::Matrix4(DirectX::XMMatrixRotationZ(0.1f * float(id + 1)));
    return node;
}

// 성공 시 부모 배열을, 실패 시 기존 출력의 보존을 검사한다.
void RunCase(const char* name, const std::vector<GraphNode>& graph,
    bool success, const std::vector<NodeId>& expected = {})
{
    // 실패 경로는 크기와 내용이 모두 다른 기존 배열로 검사한다.
    const std::vector<NodeId> original = { 17, 23, 42 };
    std::vector<NodeId> parents = original;
    Check(BuildPoseParents(graph.data(), uint32_t(graph.size()), parents) == success, name);
    Check(parents == (success ? expected : original), name);
    std::printf("PASS: %s\n", name);
}

// 부모 관계가 실제 평가기의 행렬 합성과 모든 성분에서 일치하는지 검사한다.
void CheckEvaluatorAgreement()
{
    // 중첩 분기와 skeletonRoot 뒤 형제 복귀 및 최상위 형제를 함께 둔다.
    const std::vector<GraphNode> graph = { Node(4, true, true), Node(1, true, true),
        Node(5), Node(0, true, true, true), Node(3), Node(2), Node(6) };
    std::vector<NodeId> parents;
    std::vector<Math::Matrix4> poses(graph.size());
    Check(BuildPoseParents(graph.data(), uint32_t(graph.size()), parents), "agreement parents");
    Check(parents == std::vector<NodeId>({ UINT32_MAX, 4, 4, 0, UINT32_MAX, 1, UINT32_MAX }),
        "agreement expected topology");
    Check(EvaluateModelPose(graph.data(), uint32_t(graph.size()), poses.data()), "agreement evaluator");

    // 평가기가 만든 부모 pose와 각 local을 합성하여 결과를 비교한다.
    for (const GraphNode& node : graph)
    {
        // 루트는 local 그대로이며 나머지는 유효 부모의 pose를 상속해야 한다.
        const NodeId parent = parents[node.matrixIdx];
        const Math::Matrix4 expected = parent == UINT32_MAX
            ? node.xform : poses[parent] * node.xform;
        DirectX::XMFLOAT4X4 actualValues;
        DirectX::XMFLOAT4X4 expectedValues;
        DirectX::XMStoreFloat4x4(&actualValues, poses[node.matrixIdx]);
        DirectX::XMStoreFloat4x4(&expectedValues, expected);
        for (unsigned row = 0; row < 4; ++row)
        {
            // 회전과 이동을 포함한 행렬의 모든 열을 비교한다.
            for (unsigned col = 0; col < 4; ++col)
                Check(std::abs(actualValues.m[row][col] - expectedValues.m[row][col]) < 1e-5f,
                    "agreement matrix element");
        }
    }
    std::printf("PASS: evaluator agreement\n");
}

// 물리 pose 고정과 자손 local 상속 및 skeletonRoot의 공간 초기화를 검사한다.
void CheckOverrides()
{
    // 비연속 matrixIdx와 분기를 포함한 그래프로 override 없는 동등성을 검사한다.
    std::vector<GraphNode> graph = { Node(4, true, true), Node(1, true, true),
        Node(5), Node(0, true, true, true), Node(3), Node(2), Node(6) };
    std::vector<NodeId> parents;
    std::vector<Math::Matrix4> expected(graph.size());
    std::vector<Math::Matrix4> actual(graph.size());
    Check(BuildPoseParents(graph.data(), uint32_t(graph.size()), parents), "override parents");
    Check(EvaluateModelPose(graph.data(), uint32_t(graph.size()), expected.data()), "baseline pose");
    Check(EvaluatePoseOverrides(graph.data(), uint32_t(graph.size()), parents, nullptr, 0,
        actual.data()), "zero overrides");
    for (uint32_t i = 0; i < graph.size(); ++i)
    {
        // 기존 평가기와 모든 성분이 정확히 같아야 한다.
        CheckMatrix(actual[i], expected[i]);
    }
    std::printf("PASS: zero overrides exact agreement\n");

    // 부모 애니메이션이 바뀌어도 물리 본과 body 없는 자손의 관계를 보존한다.
    const NodePhysicsPose physics = { 1, expected[1] };
    const Math::Matrix4 childLocal = graph[2].xform;
    graph[0].xform = Math::Matrix4(DirectX::XMMatrixTranslation(40.0f, -8.0f, 12.0f));
    graph[2].xform = Math::Matrix4(DirectX::XMMatrixRotationY(0.7f)) * childLocal;
    const std::vector<GraphNode> original = graph;
    Check(EvaluatePoseOverrides(graph.data(), uint32_t(graph.size()), parents, &physics, 1,
        actual.data()), "moved animation parent");
    CheckMatrix(actual[4], Math::Matrix4(Math::kIdentity) * graph[0].xform);
    CheckMatrix(actual[1], physics.ModelPose);
    CheckMatrix(actual[5], physics.ModelPose * graph[2].xform);
    for (uint32_t i = 0; i < graph.size(); ++i)
    {
        // 공유 그래프의 local 애니메이션은 수정하지 않아야 한다.
        CheckMatrix(graph[i].xform, original[i].xform);
    }
    std::printf("PASS: fixed physics bone and animated body-less child\n");

    // skeletonRoot는 물리 조상의 영향을 끊고 자신의 자손에는 pose를 전달한다.
    const NodePhysicsPose ancestorPhysics = { 4, physics.ModelPose };
    Check(EvaluatePoseOverrides(graph.data(), uint32_t(graph.size()), parents, &ancestorPhysics, 1,
        actual.data()), "physics ancestor of skeleton root");
    CheckMatrix(actual[0], graph[3].xform);
    CheckMatrix(actual[3], graph[3].xform * graph[4].xform);
    const NodePhysicsPose rootPhysics = { 0, physics.ModelPose };
    Check(EvaluatePoseOverrides(graph.data(), uint32_t(graph.size()), parents, &rootPhysics, 1,
        actual.data()), "overridden skeleton root");
    CheckMatrix(actual[0], rootPhysics.ModelPose);
    CheckMatrix(actual[3], rootPhysics.ModelPose * graph[4].xform);
    std::printf("PASS: skeleton root reset and override\n");
}

// 거절되는 입력마다 출력 배열 전체가 그대로 남는지 검사한다.
void CheckOverrideFailure(const char* name, const GraphNode* graph, uint32_t count,
    const std::vector<NodeId>& parents, const NodePhysicsPose* overrides,
    uint32_t overrideCount, bool nullOutput = false)
{
    // 부분 쓰기를 탐지하도록 식별 가능한 서로 다른 행렬을 준비한다.
    std::vector<Math::Matrix4> output = { Node(7).xform, Node(8).xform, Node(9).xform };
    const std::vector<Math::Matrix4> original = output;
    Check(!EvaluatePoseOverrides(graph, count, parents, overrides, overrideCount,
        nullOutput ? nullptr : output.data()), name);
    for (uint32_t i = 0; i < output.size(); ++i)
    {
        // 마지막 출력 원소까지 실패 전 값을 유지해야 한다.
        CheckMatrix(output[i], original[i]);
    }
    std::printf("PASS: %s\n", name);
}

// 포인터와 부모 정보 및 override 행렬의 모든 거절 경로를 검사한다.
void CheckOverrideRejections()
{
    // 유효 입력을 기준으로 한 항목씩 잘못된 값을 전달한다.
    const std::vector<GraphNode> graph = { Node(2, true), Node(0, true), Node(1) };
    const std::vector<NodeId> parents = { 2, 0, UINT32_MAX };
    NodePhysicsPose poses[] = { { 0, Node(4).xform }, { 0, Node(5).xform } };
    CheckOverrideFailure("parent size mismatch", graph.data(), 3, {}, nullptr, 0);
    CheckOverrideFailure("parent out of range", graph.data(), 3, { 3, 0, UINT32_MAX }, nullptr, 0);
    CheckOverrideFailure("parent topology mismatch", graph.data(), 3, { 1, 0, UINT32_MAX }, nullptr, 0);
    CheckOverrideFailure("null override graph", nullptr, 3, parents, nullptr, 0);
    CheckOverrideFailure("empty override graph", graph.data(), 0, {}, nullptr, 0);
    CheckOverrideFailure("null overrides", graph.data(), 3, parents, nullptr, 1);
    CheckOverrideFailure("null final output", graph.data(), 3, parents, nullptr, 0, true);
    CheckOverrideFailure("duplicate override", graph.data(), 3, parents, poses, 2);
    poses[1].Node = 3;
    CheckOverrideFailure("override out of range", graph.data(), 3, parents, poses, 2);

    // 앞선 override가 유효해도 뒤의 비유한 행렬 때문에 출력하면 안 된다.
    poses[1].Node = 1;
    poses[1].ModelPose.SetX(Math::Vector4(DirectX::XMVectorReplicate(
        std::numeric_limits<float>::quiet_NaN())));
    CheckOverrideFailure("NaN override", graph.data(), 3, parents, poses, 2);
    poses[1].ModelPose.SetX(Math::Vector4(DirectX::XMVectorReplicate(
        std::numeric_limits<float>::infinity())));
    CheckOverrideFailure("infinite override", graph.data(), 3, parents, poses, 2);

    // BC-01 검증도 평가기에서 실행하여 잘못된 DFS의 부분 출력을 방지한다.
    const std::vector<GraphNode> duplicate = { Node(0, true), Node(0), Node(1) };
    const std::vector<GraphNode> early = { Node(2), Node(0), Node(1) };
    CheckOverrideFailure("override duplicate graph index", duplicate.data(), 3, parents, nullptr, 0);
    CheckOverrideFailure("override early graph termination", early.data(), 3, parents, nullptr, 0);
}

// 정상 토폴로지와 잘못된 입력 및 형제 스택 경계를 CPU에서 검증한다.
int main()
{
    // DFS 순서와 다른 식별자 및 루트와 형제의 부모 관계를 검사한다.
    RunCase("single root", { Node(0) }, true, { UINT32_MAX });
    RunCase("shuffled indices", { Node(2, true), Node(0, true), Node(1) }, true,
        { 2, 0, UINT32_MAX });
    RunCase("siblings", { Node(0, true, true), Node(1, false, true), Node(2), Node(3) }, true,
        { UINT32_MAX, 0, 0, UINT32_MAX });
    RunCase("skeleton root", { Node(0, true), Node(1, true, false, true), Node(2) }, true,
        { UINT32_MAX, UINT32_MAX, 1 });

    // 잘못된 식별자와 조기 종료 및 미완료 순회를 거절한다.
    RunCase("duplicate index", { Node(0, true), Node(0) }, false);
    RunCase("out of range", { Node(1) }, false);
    RunCase("early termination", { Node(0), Node(1) }, false);
    RunCase("missing child", { Node(0, true) }, false);
    RunCase("missing sibling", { Node(0, false, true) }, false);
    RunCase("pending stack", { Node(0, true, true), Node(1) }, false);
    RunCase("empty graph", {}, false);

    // 널 포인터도 기존 출력을 보존하며 거절해야 한다.
    std::vector<NodeId> parents = { 91 };
    Check(!BuildPoseParents(nullptr, 1, parents), "null graph");
    Check(parents == std::vector<NodeId>({ 91 }), "null output unchanged");
    std::printf("PASS: null graph\n");

    // 32개 분기를 저장하고 역순으로 형제들을 방문하는 정상 경계를 만든다.
    std::vector<GraphNode> boundary;
    std::vector<NodeId> expected(65, UINT32_MAX);
    for (uint32_t i = 0; i < 32; ++i)
    {
        // 각 분기의 첫 자식은 직전 노드를 부모로 갖는다.
        boundary.push_back(Node(i, true, true));
        expected[i + 1] = i;
    }

    // 경계의 마지막 자식과 저장된 형제 노드들을 추가한다.
    boundary.push_back(Node(32));
    for (uint32_t i = 33; i < 65; ++i)
    {
        // 저장된 부모는 한 단계씩 복원되고 마지막 형제는 최상위다.
        boundary.push_back(Node(i));
        expected[i] = i == 64 ? UINT32_MAX : 63 - i;
    }

    // 완성된 경계 그래프의 부모 관계를 검사한다.
    RunCase("stack capacity 32", boundary, true, expected);

    // 33번째 저장은 거절하되 형제 없는 깊은 체인은 평가기처럼 허용한다.
    boundary[32] = Node(32, true, true);
    RunCase("stack overflow", boundary, false);
    std::vector<GraphNode> chain;
    expected.assign(40, UINT32_MAX);
    for (uint32_t i = 0; i < 40; ++i)
    {
        // 형제가 없으면 스택을 소비하지 않는 규칙을 고정한다.
        chain.push_back(Node(i, i + 1 < 40));
        expected[i] = i == 0 ? UINT32_MAX : i - 1;
    }

    // 완성된 깊은 체인의 부모 관계를 검사한다.
    RunCase("deep chain without siblings", chain, true, expected);

    // 독립적인 행렬 평가 결과와 대조하고 전체 검사 수를 출력한다.
    CheckEvaluatorAgreement();

    // BC-02 물리 pose 합성과 실패 시 출력 보존을 검사한다.
    CheckOverrides();
    CheckOverrideRejections();

    // 모든 검사를 통과한 최종 결과를 출력한다.
    std::printf("SkeletonPoseTests: 32 tests PASS\n");
    return 0;
}
