#pragma once

// 절단 데모의 상태와 흐름 (그리기 없음): 평면, 등록된 대상, 선분별 절단 작업. 설계: Docs/PerSegmentCutDesign.md.

#include "CutJob.h"
#include "../Geometry/CutPlane.h"
#include "../Geometry/Skinning.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Model;
class ModelInstance;

namespace MeshCutting
{
    // 한 단계의 시간 (ms): 이번 프레임 값(대상·팔레트·작업이 여럿이면 합)과 지수 평균. 평균에는 그 단계를 건너뛴 프레임의 0도 들어간다.
    struct CutPhaseTime
    {
        float frame = 0.0f;
        float average = 0.0f;
    };

    // 작업 시간 합계(예전 값)와 계측값. 개수는 이번 프레임 값이다. 교차 찾기, 추출, 절단은 마지막 값, 스키닝은 작업별 지수 평균의 합이다 (ms).
    struct CutFrameStats
    {
        uint32_t jobs = 0;
        uint32_t candidates = 0;
        uint32_t confirmed = 0;
        uint32_t extractCount = 0;      // 지금까지 실제로 추출한 횟수 (형제가 공유한 작업은 세지 않는다)
        float pickMs = 0.0f;            // 교차 삼각형 찾기 (마지막으로 다시 계산한 시간)
        float extractMs = 0.0f;
        float skinMs = 0.0f;
        float cutMs = 0.0f;

        // 이번 프레임 개수
        uint32_t targets = 0;
        uint32_t palettes = 0;
        uint32_t cacheVertices = 0;     // 캐시 정점의 현재 위치를 계산한 수
        uint32_t jobVertices = 0;       // 작업의 패치·조각 정점을 스키닝한 수
        uint32_t staticMeshes = 0;      // 스킨 없는 메시 수
        uint32_t staticCulled = 0;      // 이번 프레임 다시 계산한 메시 중 바운딩 구 조기 판정으로 건너뛴 수
        uint32_t staticTriangles = 0;   // 스킨 없는 메시에서 정밀 검사한 삼각형 수
        uint32_t checkedTriangles = 0;
        uint32_t mixedTriangles = 0;    // 평면 양쪽에 걸친 삼각형
        uint32_t crossedTriangles = 0;  // 절단 joint를 고른 교차 삼각형
        uint32_t jobsAdded = 0, jobsKept = 0, jobsLost = 0;
        uint32_t extractions = 0;
        uint32_t refreshPose = 0, refreshPlane = 0, refreshBoth = 0, refreshSkipped = 0;    // 팔레트별 교차 재계산 이유

        // 단계별 시간
        CutPhaseTime skeleton;          // ComputePaletteSkeleton
        CutPhaseTime poseMatrices;      // 교차용 스킨 행렬 + 자세 변경 판단
        CutPhaseTime cacheSkin;         // 캐시 정점 스키닝
        CutPhaseTime pick;              // PickJoints
        CutPhaseTime segments;          // 뼈 선분 표시용 PickSegments
        CutPhaseTime jobUpdate;         // key 만들기 + DiffJobs + ApplyDiff
        CutPhaseTime extract;           // Enter 때 작업의 추출·용접
        CutPhaseTime jobMatrices;       // 작업의 스킨 행렬
        CutPhaseTime jobSkin;           // 작업의 확정 조각 스키닝 + Enter 때 패치 스키닝
        CutPhaseTime cut;               // Enter의 절단 (SliceWithPlane)
        CutPhaseTime confirm;           // Enter의 ConfirmAll
    };

    class CutController
    {
    public:
        // 등록한 인스턴스는 UnregisterInstance 전까지 살아 있어야 한다. 해제하면 그 대상의 작업이 모두 지워진다.
        // 스태틱 메시 절단이 확정되면 컨트롤러가 인스턴스의 원본 메시를 숨기므로 non-const로 받는다.
        CutTargetId RegisterInstance(ModelInstance& instance, std::wstring name);
        void UnregisterInstance(CutTargetId id);

        void ToggleEditPlane();     // T: 첫 대상의 bounds 중심에 평면을 놓는다 (처음 한 번)
        void SetGizmoTool(GizmoTool tool) { m_Plane.SetGizmoTool(tool); }
        bool UpdateGizmo(const GizmoView& view, const GizmoInput& input);   // 드래그로 평면이 바뀌면 true
        void CancelGizmo() { m_Plane.CancelGizmo(); }
        void RequestCut();          // Enter: 다음 Update에서 이전 확정 결과를 지우고 후보 작업마다 절단
        void DiscardCut();          // Ctrl+Z: 모든 확정 결과를 지운다

        // 모든 인스턴스의 Update 뒤에 한 번: 메시 교차 -> 작업 목록 -> 스키닝 -> Enter 요청 시 확정
        void Update();

        const CutPlane& GetPlane() const { return m_Plane; }
        bool PlaneEditing() const { return m_PlaneEditing; }
        bool PlaneValid() const { return m_PlaneValid; }
        float GetModelRadius() const { return m_ModelRadius; }      // 평면 조작과 디버그 tube 크기의 기준

        const std::vector<CutTarget>& GetTargets() const { return m_Targets; }
        const CutTarget* FindTarget(CutTargetId id) const;
        const std::map<CutJobKey, CutJob>& GetJobs() const { return m_Jobs; }
        uint64_t GetJobListGeneration() const { return m_JobListGeneration; }      // 작업이 생기거나 지워질 때 증가
        CutFrameStats GetFrameStats() const;

        // 웨이트 뷰: 대상의 팔레트별 joint -> 작업 색 표. 후보 작업의 부모 joint가 그 작업의 색을 갖고(형제는 먼저 생긴 작업의 색),
        // 나머지 joint는 GetNoJobColor이다. 그 팔레트에 후보 작업이 없으면 false.
        bool GetJointColors(CutTargetId target, uint32_t palette, std::vector<DirectX::XMFLOAT4>& colors) const;

    private:
        void ReloadPalettes(CutTarget& target);
        void BuildSource(CutTarget& target, size_t slot);
        void RefreshPick(CutTarget& target, size_t slot, bool planeChanged);
        void BuildStaticCache(const CutTarget& target, CutStaticMesh& mesh);
        void PickStaticMeshes(CutTarget& target, bool planeChanged, std::vector<CutJobKey>& current, std::map<CutJobKey, uint32_t>& triangles);
        void RemoveJobsOf(CutTargetId id);
        void SetOriginalVisible(const CutJob& job, bool visible);
        void UpdateSkinMask(CutTarget& target);
        void UpdateTargets();
        void UpdateCandidates();
        void ApplyDiff(const CutJobDiff& diff, const std::map<CutJobKey, uint32_t>& triangles);
        void ExtractJob(CutJob& job);
        void DropCandidate(CutJob& job);
        void SkinJobs();
        void ConfirmAll();
        void ConfirmJob(CutJob& job);
        bool SkinPatch(CutJob& job, std::string& error);
        SliceStatus SliceWithPlane(const CutJob& job, SliceResult& result) const;
        void PlacePlane(const ModelInstance& instance);

        CutPlane m_Plane;
        bool m_PlaneEditing = false;
        bool m_PlaneValid = false;
        bool m_CutRequested = false;
        float m_ModelRadius = 0.0f;

        std::vector<CutTarget> m_Targets;
        CutTargetId m_NextTargetId = 1;
        std::map<CutJobKey, CutJob> m_Jobs;
        uint64_t m_JobListGeneration = 0;
        uint32_t m_NextColorIndex = 0;
        uint32_t m_ExtractCount = 0;
        uint64_t m_Generation = 0;          // 확정 결과의 세대 번호를 모든 작업에서 유일하게 만든다
        CutPlane::Frame m_PickedFrame;         // 마지막으로 교차를 찾은 평면
        bool m_HavePickedFrame = false;
        float m_PickMs = 0.0f;
        CutFrameStats m_Frame;                // 이번 프레임 개수와 단계별 시간 (평균은 프레임을 넘어 유지)
    };
}
