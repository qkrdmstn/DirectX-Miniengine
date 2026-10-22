#pragma once

// 평면과 교차하는 삼각형을 찾아 삼각형마다 절단 joint 하나를 고른다. Model, Core에 의존하지 않는다 (설계: Docs/JointCutDesign.md 2.1, 2.2절).

#include "CutPlane.h"
#include "Skinning.h"
#include <cstdint>
#include <vector>

namespace MeshCutting
{
    // 한 대상·팔레트의 스킨 메시를 현재 자세로 본 모습
    struct CutPickMesh
    {
        std::vector<DirectX::XMFLOAT3> positions;       // 정점의 현재 world 위치
        std::vector<SkinVertex> skins;                  // 같은 순서의 joint와 가중치 (positions와 크기가 같아야 하고, 중복 joint가 있어도 된다)
        std::vector<uint32_t> indices;                  // 삼각형 3개씩
    };

    struct CutPickJoint
    {
        uint32_t joint = kNoNode;           // 팔레트 안 joint 번호
        uint32_t triangleCount = 0;         // 이 joint를 고른 교차 삼각형 수
    };

    // PickJoints가 센 삼각형 수 (계측용, 삼각형 반복 안에서는 정수 증가만 한다)
    struct CutPickStats
    {
        uint32_t checkedTriangles = 0;      // 훑은 삼각형
        uint32_t mixedTriangles = 0;        // 평면 양쪽에 걸친 삼각형 (사각형 검사 전)
        uint32_t crossedTriangles = 0;      // 절단선이 사각형과 만나고 절단 joint를 고른 삼각형
    };

    class CutPick
    {
    public:
        // 교차 삼각형마다 절단 joint를 골라, joint별 교차 삼각형 수를 joint 오름차순으로 돌려준다.
        // jointParents[joint]는 팔레트 안 부모 joint (루트는 kNoNode). minWeight를 넘는 가중치의 joint만 후보다.
        // epsilon 안의 정점은 평면 위로 본다. 평면이 올바르지 않거나 mesh의 크기가 맞지 않으면 out은 비어 있다.
        static void PickJoints(const CutPickMesh& mesh, const std::vector<uint32_t>& jointParents,
            const CutPlane& plane, float minWeight, float epsilon, std::vector<CutPickJoint>& out, CutPickStats* stats = nullptr);

        // 스킨 없는 메시용: 로컬 정점의 교차 삼각형 수만 센다 (PickJoints와 같은 부호 규칙과 사각형 검사). 정점은 world로 옮기지 않는다.
        // 평면을 로컬로 옮겨 부호를 보고, 교차 삼각형의 절단선 끝점만 localToWorld(행 벡터 규약)로 옮겨 사각형과 비교한다.
        // 평면이 올바르지 않으면 0.
        static uint32_t CountCrossedTriangles(const std::vector<DirectX::XMFLOAT3>& localPositions, const std::vector<uint32_t>& indices,
            const DirectX::XMFLOAT4X4& localToWorld, const CutPlane& plane, float epsilon, CutPickStats* stats = nullptr);

        // 로컬 바운딩 구(x, y, z, 반지름)를 world로 옮겨 평면 사각형과 비교한다 (Unreal 섹션 박스 조기 판정에 대응).
        // 구가 사각형 확장 범위와 만나지 않으면 false: 이때만 그 메시를 건너뛰어도 된다. 구를 믿을 수 없으면 true.
        static bool SphereMayCross(const DirectX::XMFLOAT4& localSphere, const DirectX::XMFLOAT4X4& localToWorld,
            const CutPlane& plane, float epsilon);
    };
}
