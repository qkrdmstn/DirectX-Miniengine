#pragma once

// joint 또는 스태틱 메시별 절단 작업의 식별자, 상태, 목록 비교. Model, Core에 의존하지 않는다.

#include "../Geometry/CutPick.h"
#include "../Geometry/CutPlane.h"
#include "../Geometry/MeshGeometry.h"
#include "../Geometry/Slice.h"
#include "../ProceduralMesh.h"
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

class Model;
class ModelInstance;

namespace MeshCutting
{
    struct MeshSource;

    using CutTargetId = uint32_t;

    enum class CutJobKind : uint8_t
    {
        SkeletalSegment,    // 스킨 팔레트의 부모 -> 자식 뼈 선분 (예전 방식, key 모양만 남아 있다)
        SkeletalJoint,      // 스킨 팔레트의 joint 하나 (평면과 교차하는 삼각형이 고른 절단 joint)
        StaticMesh          // 스킨 없는 메시 하나 (a = Model 메시 번호)
    };

    // 작업 식별자. 사전식 비교로 std::map의 key가 된다.
    struct CutJobKey
    {
        CutTargetId target = 0;
        CutJobKind kind = CutJobKind::SkeletalSegment;
        uint32_t a = kInvalidIndex;   // SkeletalSegment: 팔레트, StaticMesh: Model 메시 번호
        uint32_t b = kInvalidIndex;   // SkeletalSegment: 부모 joint
        uint32_t c = kInvalidIndex;   // SkeletalSegment: 자식 joint

        bool operator<(const CutJobKey& other) const
        {
            if (target != other.target) return target < other.target;
            if (kind != other.kind) return kind < other.kind;
            if (a != other.a) return a < other.a;
            if (b != other.b) return b < other.b;
            return c < other.c;
        }
        bool operator==(const CutJobKey& other) const
        {
            return target == other.target && kind == other.kind && a == other.a && b == other.b && c == other.c;
        }
    };

    inline CutJobKey MakeSegmentKey(CutTargetId target, uint32_t palette, uint32_t parentJoint, uint32_t childJoint)
    {
        CutJobKey key;
        key.target = target;
        key.kind = CutJobKind::SkeletalSegment;
        key.a = palette;
        key.b = parentJoint;
        key.c = childJoint;
        return key;
    }

    inline CutJobKey MakeJointKey(CutTargetId target, uint32_t palette, uint32_t joint)
    {
        CutJobKey key;
        key.target = target;
        key.kind = CutJobKind::SkeletalJoint;
        key.a = palette;
        key.b = joint;
        return key;
    }

    inline CutJobKey MakeStaticMeshKey(CutTargetId target, uint32_t meshIndex)
    {
        CutJobKey key;
        key.target = target;
        key.kind = CutJobKind::StaticMesh;
        key.a = meshIndex;
        return key;
    }

    struct CutJob
    {
        CutJobKey key;
        std::shared_ptr<const MeshSource> source;
        uint32_t colorIndex = 0;            // 생성 순서로 정한 고정 색 번호 (GetJobColor)

        bool candidate = false;             // 이번 프레임에 평면이 이 작업을 지나는가
        uint32_t pickTriangles = 0;         // 이 joint를 고른 교차 삼각형 수

        uint32_t patchTriangles = 0;        // 마지막 Enter에서 추출한 패치의 삼각형 수
        WeldStats weld;
        SliceStatus patchStatus = SliceStatus::InvalidInput;        // 마지막 Enter의 추출 결과
        std::string patchError;
        std::vector<std::vector<DirectX::XMFLOAT4X4>> skinMatrices;     // 이번 프레임의 스킨 행렬
        bool poseValid = false;             // 이번 프레임 조각 스키닝이 끝났는가
        std::string skinError;

        bool haveConfirmed = false;         // 확정 결과 (후보가 사라져도 유지, Ctrl+Z나 다음 Enter에서 지워진다)
        // The view retains these same objects until GPU registrations are released.
        std::shared_ptr<ProceduralMesh> positive, negative;
        std::vector<std::vector<DirectX::XMFLOAT4X4>> appliedMatrices[2];     // 조각(positive, negative)마다 마지막으로 UpdatePose에 성공한 스킨 행렬 (조각이 바뀌면 비운다)
        // 스태틱 작업만: 조각 정점은 절단 순간 world에 고정하고, 이후에는 행렬로만 옮긴다 (Unreal 컴포넌트 트랜스폼처럼)
        DirectX::XMFLOAT4X4 cutWorld = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };      // 절단 순간의 메시 world
        DirectX::XMFLOAT4X4 pieceWorld = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };    // 지금 world × inverse(cutWorld). 그릴 때 곱한다
        DirectX::XMFLOAT3 confirmedNormal = { 0.0f, 1.0f, 0.0f };
        uint64_t confirmedGeneration = 0;
        uint32_t breakNode = kInvalidIndex;       // 확정 때 끊을 joint의 장면 그래프 노드 (스켈레탈 joint 작업만, U-5a)
        DirectX::XMFLOAT3 breakOffset = { 0.0f, 0.0f, 0.0f };     // 끊은 노드를 옮길 world 이동량 (떨어지는 쪽 판정용)
        DirectX::XMFLOAT3 breakOffsetNode = { 0.0f, 0.0f, 0.0f };     // 같은 이동량을 노드 행렬(m_NodeWorld) 공간으로 바꾼 값 (BreakConstraint 입력)
        // 끊긴 joint의 절단 순간 값 (U-5b, breakNode가 있을 때만): 조각이 양쪽을 따라가게 joint 행렬을 교체하는 데 쓴다.
        std::vector<std::vector<DirectX::XMFLOAT4X4>> breakCutMatrices;     // 메시 world를 뺀 slot 행렬 (inverse(meshWorld) * slot)
        DirectX::XMFLOAT4X4 breakNodeWorld, breakParentWorld;       // 절단 순간 끊긴 노드와 그 부모의 world
        bool breakHasParent = false;
        uint32_t breakParent = kInvalidIndex;
        std::vector<uint8_t> breakDetached;     // 팔레트 joint가 끊긴 노드이거나 그 자손인가
        bool positiveFalls = false;             // positive 조각이 떨어지는 쪽인가
        bool cutAttempted = false;          // Ctrl+Z 뒤 Enter를 처리했는가
        SliceStatus lastCutStatus = SliceStatus::InvalidInput;      // 마지막 Enter의 결과
        std::string lastCutError;
        std::wstring cutInfo;
        float cutMs = 0.0f;

        float extractMs = 0.0f;
        float skinMs = 0.0f;
    };

    // 대상의 팔레트 하나의 원본 캐시: 등록 때 팔레트의 모든 joint로 추출한 스킨 메시와, 이번 프레임의 교차 결과
    struct CutTargetSource
    {
        std::shared_ptr<const MeshSource> source;      // 전체 추출의 출처 (vertices, meshes)
        CutPickMesh pose;                                   // 정점의 현재 위치 (skins는 등록 때 채운다)
        std::vector<std::vector<DirectX::XMFLOAT4X4>> matrices;     // pose를 계산한 스킨 행렬 (자세 변화 감지)
        std::vector<CutPickJoint> picked;                   // 마지막 교차 결과
        bool attempted = false;                             // 원본 캐시를 만들어 보았는가 (실패하면 편집 모드를 다시 켤 때 재시도)
        std::wstring error;
    };

    // 대상의 스킨 없는 메시 하나: 후보 찾기용 로컬 캐시 (편집을 처음 시작할 때 한 번 만든다)
    struct CutStaticMesh
    {
        uint32_t meshIndex = 0;                         // Model 메시 번호
        uint16_t meshCBV = 0;                           // world 행렬 번호
        DirectX::XMFLOAT4 bounds = { 0.0f, 0.0f, 0.0f, 0.0f };     // 로컬 바운딩 구 (x, y, z, 반지름)
        std::vector<DirectX::XMFLOAT3> positions;       // 로컬 위치 (CopyProceduralMeshFromStaticMesh의 정점)
        std::vector<uint32_t> indices;                  // 삼각형 3개씩
        bool attempted = false;                         // 캐시를 만들어 보았는가
        DirectX::XMFLOAT4X4 world;                      // 마지막으로 교차를 찾은 world 행렬
        bool havePicked = false;
        uint32_t crossed = 0;                           // 마지막 교차 삼각형 수
        std::wstring error;
    };

    // 등록된 모델 인스턴스. instance는 UnregisterInstance 전까지 유효해야 한다.
    struct CutTarget
    {
        CutTargetId id = 0;
        ModelInstance* instance = nullptr;
        const Model* model = nullptr;               // 등록 때의 Model (바뀌면 작업을 모두 지운다)
        std::wstring name;
        std::vector<uint32_t> palettes;             // numJoints > 0인 스킨 팔레트 전부
        std::vector<SkeletonPose> skeletons;        // palettes와 같은 순서, 이번 프레임 자세
        std::vector<CutTargetSource> sources;       // palettes와 같은 순서
        std::vector<std::vector<SegmentHit>> segmentHits;       // palettes와 같은 순서, 평면이 지나는 뼈 선분 (표시용)
        std::vector<CutStaticMesh> staticMeshes;    // 스킨 없는 메시 전부
        std::set<uint32_t> maskedMeshes;            // 버텍스 컬러 마스크를 덮어쓴 스킨 메시 번호 (U-4b)
        std::set<uint32_t> brokenNodes;             // BreakConstraint로 끊어 둔 노드 번호 (U-5a)
        std::wstring error;                         // 마지막 자세 읽기 오류
        float radius = 0.0f;                        // 표시 오프셋과 tube 크기
    };

    struct CutJobDiff
    {
        std::vector<CutJobKey> added;       // 새 후보 작업 (없거나 후보가 아니던 작업)
        std::vector<CutJobKey> lost;        // 후보였다가 사라진 작업
        std::vector<CutJobKey> kept;        // 계속 후보인 작업
    };

    // 이번 프레임 key 목록(current)을 기존 작업과 비교한다. 같은 key가 여러 번 나오면 첫 번째만 본다.
    inline CutJobDiff DiffJobs(const std::map<CutJobKey, CutJob>& jobs, const std::vector<CutJobKey>& current)
    {
        CutJobDiff diff;
        std::map<CutJobKey, bool> seen;
        for (const CutJobKey& key : current)
        {
            if (!seen.emplace(key, true).second)
                continue;
            const auto found = jobs.find(key);
            if (found != jobs.end() && found->second.candidate)
                diff.kept.push_back(key);
            else
                diff.added.push_back(key);
        }
        for (const auto& entry : jobs)
            if (entry.second.candidate && seen.find(entry.first) == seen.end())
                diff.lost.push_back(entry.first);
        return diff;
    }

    constexpr uint32_t kJobColorCount = 8;

    // 어느 작업에도 속하지 않은 joint의 색 (회색, 웨이트 뷰)
    inline DirectX::XMFLOAT4 GetNoJobColor() { return { 0.12f, 0.12f, 0.12f, 1.0f }; }

    // 색 번호 -> 작업 색 (8색 순환). 웨이트 뷰, 미리보기 루프, 조각 표시가 같은 색을 쓴다.
    inline DirectX::XMFLOAT4 GetJobColor(uint32_t colorIndex)
    {
        static const DirectX::XMFLOAT4 colors[kJobColorCount] = {
            { 1.00f, 0.00f, 0.00f, 1.0f }, { 0.00f, 0.35f, 1.00f, 1.0f }, { 0.00f, 1.00f, 0.15f, 1.0f },
            { 1.00f, 0.90f, 0.00f, 1.0f }, { 1.00f, 0.00f, 1.00f, 1.0f }, { 0.00f, 1.00f, 1.00f, 1.0f },
            { 1.00f, 0.45f, 0.00f, 1.0f }, { 0.55f, 0.00f, 1.00f, 1.0f } };
        return colors[colorIndex % kJobColorCount];
    }
}
