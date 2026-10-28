// Worktree 루트의 VS Developer PowerShell에서 빌드하고 실행한다.
// $bcTestDir = Join-Path $env:TEMP 'BreakConstraintTests'
// New-Item -ItemType Directory -Force -Path $bcTestDir | Out-Null
// cl /nologo /std:c++17 /EHsc /W4 /utf-8 /I MiniEngine/Core /I MiniEngine/Model "/Fo:$bcTestDir/SkeletonPoseTests.obj" "/Fe:$bcTestDir/SkeletonPoseTests.exe" MiniEngine/Model/tests/SkeletonPoseTests.cpp
// & "$bcTestDir/SkeletonPoseTests.exe"

#include "SkeletonPose.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

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

    // 모든 검사를 통과한 최종 결과를 출력한다.
    std::printf("SkeletonPoseTests: 16 tests PASS\n");
    return 0;
}
