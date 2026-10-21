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
    // 작업 시간 합계. 교차 찾기, 추출, 절단은 마지막 값, 스키닝과 미리보기는 작업별 지수 평균의 합이다 (ms).
    struct CutFrameStats
    {
        uint32_t jobs = 0;
        uint32_t candidates = 0;
        uint32_t confirmed = 0;
        uint32_t extractCount = 0;      // 지금까지 실제로 추출한 횟수 (형제가 공유한 작업은 세지 않는다)
        float pickMs = 0.0f;            // 교차 삼각형 찾기 (마지막으로 다시 계산한 시간)
        float extractMs = 0.0f;
        float skinMs = 0.0f;
        float previewMs = 0.0f;
        float cutMs = 0.0f;
    };

    class CutController
    {
    public:
        // 등록한 인스턴스는 UnregisterInstance 전까지 살아 있어야 한다. 해제하면 그 대상의 작업이 모두 지워진다.
        CutTargetId RegisterInstance(const ModelInstance& instance, std::wstring name);
        void UnregisterInstance(CutTargetId id);

        void ToggleEditPlane();     // T: 첫 대상의 bounds 중심에 평면을 놓는다 (처음 한 번)
        void SetGizmoTool(GizmoTool tool) { m_Plane.SetGizmoTool(tool); }
        bool UpdateGizmo(const GizmoView& view, const GizmoInput& input);   // 드래그로 평면이 바뀌면 true
        void CancelGizmo() { m_Plane.CancelGizmo(); }
        void RequestCut();          // Enter: 다음 Update에서 이전 확정 결과를 지우고 후보 작업마다 절단
        void DiscardCut();          // Ctrl+Z: 모든 확정 결과를 지운다

        // 모든 인스턴스의 Update 뒤에 한 번: 교차 선분 -> 작업 목록 -> 스키닝 -> 미리보기 또는 확정
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
        void RemoveJobsOf(CutTargetId id);
        void UpdateTargets();
        void UpdateCandidates();
        void ApplyDiff(const CutJobDiff& diff, const std::map<CutJobKey, uint32_t>& triangles);
        void ExtractJob(CutJob& job);
        void DropCandidate(CutJob& job);
        void SkinJobs();
        void RunPreview(CutJob& job);
        void ConfirmAll();
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
        uint64_t m_Generation = 0;          // 미리보기와 확정 결과의 세대 번호를 모든 작업에서 유일하게 만든다
        CutPlane::Frame m_PickedFrame;         // 마지막으로 교차를 찾은 평면
        bool m_HavePickedFrame = false;
        float m_PickMs = 0.0f;
    };
}
