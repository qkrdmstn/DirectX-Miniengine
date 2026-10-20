#pragma once

// 원본 스켈레톤 메시 중 특정 본 주변의 메시를 가져오는 기능을 담은 파일입니다.

#include "ProceduralMesh.h"
#include "Geometry/PatchWeld.h"
#include "Geometry/PieceSkinning.h"
#include "VectorMath.h"
#include <cstdint>
#include <string>
#include <vector>

class Model;
class ModelInstance;
struct Joint;

namespace MeshCutting
{
    // 패치의 원본 메시 정보를 모델의 메시 목록을 기준으로 기록합니다.
    struct BonePatchMesh
    {
        uint32_t meshIndex;         // 원본 모델의 메시 인덱스
        uint32_t worldMatrixIndex;  // 원본 메시가 사용하는 월드 행렬의 인덱스
        uint32_t startJoint;        // 팔레트가 시작하는 위치
    };

    // ProceduralMeshData::vertices와 같은 순서로 대응합니다.
    // 패치가 나중에도 원본 정점과 같은 방식으로 스키닝될 수 있도록 보관한다.
    struct BonePatchVertex
    {
        uint32_t meshSlot;
        uint32_t sourceVertex;      // 원본 정점의 원본 버퍼에서의 인덱스
        DirectX::XMFLOAT3 position;
        DirectX::XMFLOAT3 normal;
        uint16_t joints[4];         // 현재 정점에 영향을 미치는 관절 목록
        float weights[4];           // 각 관절에 대한 가중치
    };

    // 패치된 삼각형 하나의 원본 정보이며, 패치 인덱스를 세 개씩 묶은 삼각형 순서와 대응한다.
    // 이를 통해 원본 삼각형 식별이 가능하다.
    struct BonePatchTriangle
    {
        uint32_t meshSlot;
        uint32_t drawIndex;
        uint32_t primitive;
    };

    // 선택한 관절에 영향을 받는 위에 정의한 메시, 정점, 삼각형에 대한 원본 정보를 모아두는 구조체
    struct BonePatchSource
    {
        // 미니엔진에서 구성해 둔 각 팔레트(관절 그룹)와 해당 팔레트에서의 관절 인덱스를 추적하기 위한 변수
        uint32_t paletteId = 0;
        uint32_t jointIndex = 0;

        float minWeight = 0.0f; // bone에 영향을 받는 삼각형을 고를 때 최소 영향력 기준

        std::vector<BonePatchMesh> meshes; //피부와 옷 같이 메시가 여러 개인 상황도 있다.
        std::vector<BonePatchVertex> vertices;
        std::vector<BonePatchTriangle> triangles;

        // 추출 시점의 모든 팔레트 관절 행렬을 패치 정점과 같은 공간으로 저장합니다.
        // 이는 절단 평면과 절단면을 관절에 연결할 때 기준으로 사용합니다.
        std::vector<Math::Matrix4> poseJointWorld;

        // Triangles left outside the patch that are weighted both to the selected joint's
        // subtree and to other joints. They would stretch once the subtree is separated, so a
        // non-zero count means the selected joint is not a clean cut candidate.
        uint32_t mixedOutsideTriangles = 0;
    };

    // 삼각형의 정점 중 하나라도 선택 관절의 가중치가 minWeight를 넘으면 삼각형을 복사한다. 선택 관절 자신만 기준으로 삼는다.
    // 자손 관절에만 영향을 받는 삼각형은 원본에 남긴다는 정책이다. 실제 분리 기능이 이 함수에 있는건 아니다.
    // 인스턴스의 마지막 Update()에서 계산한 행렬로 자세를 적용한다.
    // 결과 위치는 월드 좌표이므로, 그릴 때 주의해야 한다.
    // 실패하면 출력 메시와 출처 정보는 변경하지 않는다.
    bool ExtractBonePatch(const Model& model, const ModelInstance& instance,
        uint32_t paletteId, uint32_t jointIndex, float minWeight,
        ProceduralMeshData& outMesh, BonePatchSource& outSource, std::string* error = nullptr);

    // 메시 하나에 대해 메시 월드 × 관절 스키닝 행렬을 계산합니다.
    // 최초 추출과 이후 매 프레임 갱신이 같은 행렬과 같은 혼합 계산을 사용합니다.
    // 공통 함수인 SkinPoint를 사용해, 같은 자세에서 두 경로의 계산을 맞춘다는 설명입니다.
    void MakeSlotMatrices(const Math::Matrix4& meshWorld, const Joint* paletteJoints, uint32_t jointCount,
        std::vector<DirectX::XMFLOAT4X4>& out);

    // slotMatrices[meshSlot] for the patch's source meshes, from the instance's current pose
    // (the pose of its last Update). meshSlot is the index into source.meshes, which is the
    // meshSlot of every SkinNode (ExtractBonePatch numbers meshes in that order).
    bool BuildSkinMatrices(const Model& model, const ModelInstance& instance, const BonePatchSource& source,
        std::vector<std::vector<DirectX::XMFLOAT4X4>>& slotMatrices, std::string* error = nullptr);

    // World matrices of the palette joints at the instance's current pose, in the same space as
    // BonePatchSource::poseJointWorld (the first source mesh's world * joint node world).
    bool ComputePoseJointWorld(const Model& model, const ModelInstance& instance, const BonePatchSource& source,
        std::vector<Math::Matrix4>& out, std::string* error = nullptr);

    // Flattens an extraction result into the plain input of Geometry/BuildPatchPiece, so the
    // slicing code never sees Model or Core types. The two inputs must come from the same
    // ExtractBonePatch call.
    bool MakeWeldInput(const ProceduralMeshData& mesh, const BonePatchSource& source,
        WeldInput& out, std::string* error = nullptr);
}
