#pragma once

// 절단 작업(선분 하나 = 독립 패치 하나)의 식별자, 상태, 목록 비교. Model, Core에 의존하지 않는다 (설계: Docs/PerSegmentCutDesign.md 4절).

#include "../Geometry/CutPlane.h"
#include "../Geometry/Patch.h"
#include "../Geometry/Slice.h"
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

class Model;
class ModelInstance;

namespace MeshCutting
{
    struct BonePatchSource;

    using CutTargetId = uint32_t;

    enum class CutJobKind : uint8_t
    {
        SkeletalSegment,    // 스킨 팔레트의 부모 -> 자식 뼈 선분
        StaticMesh          // 스킨 없는 메시 하나 (구조만)
    };

    // 작업 식별자. 사전식 비교로 std::map의 key가 된다.
    struct CutJobKey
    {
        CutTargetId target = 0;
        CutJobKind kind = CutJobKind::SkeletalSegment;
        uint32_t a = kNoNode;   // SkeletalSegment: 팔레트, StaticMesh: Model 메시 번호
        uint32_t b = kNoNode;   // SkeletalSegment: 부모 joint
        uint32_t c = kNoNode;   // SkeletalSegment: 자식 joint

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

    inline CutJobKey MakeStaticMeshKey(CutTargetId target, uint32_t meshIndex)
    {
        CutJobKey key;
        key.target = target;
        key.kind = CutJobKind::StaticMesh;
        key.a = meshIndex;
        return key;
    }

    // 작업이 어떤 원본에서 왔는가. bone은 같은 부모 joint의 형제 작업끼리 공유한다.
    struct CutJobSource
    {
        CutJobKind kind = CutJobKind::SkeletalSegment;
        std::shared_ptr<const BonePatchSource> bone;
        uint32_t extractJoint = kNoNode;    // 추출에 쓴 joint (부모 joint)
    };

    struct CutJob
    {
        CutJobKey key;
        CutJobSource source;
        uint32_t colorIndex = 0;            // 생성 순서로 정한 고정 색 번호 (GetJobColor)

        bool candidate = false;             // 이번 프레임에 평면이 이 작업을 지나는가
        SegmentHit hit;                     // 마지막 교차 (표시용)

        bool extracted = false;             // 후보가 되어 추출을 마쳤는가 (실패 포함)
        PatchPiece patch;                   // 미절단 패치 (후보일 때만 유지)
        uint32_t patchTriangles = 0;
        WeldStats weld;
        SliceStatus patchStatus = SliceStatus::InvalidInput;
        std::string patchError;
        float patchRadius = 0.0f;
        std::vector<std::vector<DirectX::XMFLOAT4X4>> skinMatrices;     // 이번 프레임의 스킨 행렬
        std::vector<std::vector<DirectX::XMFLOAT4X4>> previousSkinMatrices;     // 지난 프레임의 스킨 행렬 (자세 변화 감지)
        bool poseValid = false;             // 이번 프레임 스키닝이 끝났는가
        std::string skinError;

        SliceResult preview;                // 미리보기 (dry-run)
        bool havePreview = false;
        SliceStatus previewStatus = SliceStatus::InvalidInput;
        bool previewDirty = true;
        uint64_t previewGeneration = 0;
        float previewMs = 0.0f;

        bool haveConfirmed = false;         // 확정 결과 (후보가 사라져도 Ctrl+Z 전까지 유지)
        PatchPiece positive, negative;
        DirectX::XMFLOAT3 confirmedNormal = { 0.0f, 1.0f, 0.0f };
        uint64_t confirmedGeneration = 0;
        bool cutAttempted = false;          // Ctrl+Z 뒤 Enter를 처리했는가
        SliceStatus lastCutStatus = SliceStatus::InvalidInput;      // 마지막 Enter의 결과
        std::string lastCutError;
        std::wstring cutInfo;
        float cutMs = 0.0f;

        float extractMs = 0.0f;
        float skinMs = 0.0f;
    };

    // 등록된 모델 인스턴스. instance는 UnregisterInstance 전까지 유효해야 한다.
    struct CutTarget
    {
        CutTargetId id = 0;
        const ModelInstance* instance = nullptr;
        const Model* model = nullptr;               // 등록 때의 Model (바뀌면 작업을 모두 지운다)
        std::wstring name;
        std::vector<uint32_t> palettes;             // numJoints > 0인 스킨 팔레트 전부
        std::vector<SkeletonPose> skeletons;        // palettes와 같은 순서, 이번 프레임 자세
        std::wstring error;                         // 마지막 자세 읽기 오류
        float radius = 0.0f;                        // 표시 오프셋과 tube 크기
    };

    struct CutJobDiff
    {
        std::vector<CutJobKey> added;       // 새로 추출할 작업 (없거나 후보가 아니던 작업)
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
