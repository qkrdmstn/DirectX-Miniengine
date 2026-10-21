#include "../pch.h"
#include "CutController.h"
#include "../BonePatch.h"
#include "../../Model/Model.h"
#include "EngineProfiling.h"
#include "SystemTime.h"
#include "Utility.h"

#include <cstring>

using namespace Math;

namespace MeshCutting
{
    namespace
    {
        const float kPickMinWeight = 0.05f;        // 절단 joint 후보가 되는 최소 가중치 (설계 2.2절)

        // 지수 평균. 값이 0이면 첫 표본으로 시작한다.
        void Smooth(float& value, float sample)
        {
            value = value == 0.0f ? sample : value + (sample - value) * 0.1f;
        }

        float ElapsedMs(int64_t start)
        {
            return float(SystemTime::TimeBetweenTicks(start, SystemTime::GetCurrentTick()) * 1000.0);
        }

        bool SameMatrices(const std::vector<std::vector<DirectX::XMFLOAT4X4>>& a, const std::vector<std::vector<DirectX::XMFLOAT4X4>>& b)
        {
            if (a.size() != b.size())
                return false;
            for (size_t i = 0; i < a.size(); ++i)
                if (a[i].size() != b[i].size() || memcmp(a[i].data(), b[i].data(), a[i].size() * sizeof(DirectX::XMFLOAT4X4)) != 0)
                    return false;
            return true;
        }

        void ClearPreview(CutJob& job, uint64_t generation)
        {
            job.preview = SliceResult();
            job.havePreview = false;
            job.previewStatus = SliceStatus::InvalidInput;
            job.previewGeneration = generation;
        }
    }

    CutTargetId CutController::RegisterInstance(const ModelInstance& instance, std::wstring name)
    {
        CutTarget target;
        target.id = m_NextTargetId++;
        target.instance = &instance;
        target.model = instance.GetModel();
        target.name = std::move(name);
        ReloadPalettes(target);
        m_Targets.push_back(std::move(target));
        return m_Targets.back().id;
    }

    void CutController::UnregisterInstance(CutTargetId id)
    {
        RemoveJobsOf(id);
        for (size_t i = 0; i < m_Targets.size(); ++i)
            if (m_Targets[i].id == id)
            {
                m_Targets.erase(m_Targets.begin() + i);
                break;
            }
    }

    void CutController::ReloadPalettes(CutTarget& target)
    {
        target.palettes.clear();
        target.skeletons.clear();
        target.error.clear();
        target.radius = float(target.instance->GetRadius());
        if (!(target.radius > 0.0f))
            target.radius = 1.0f;
        if (target.model == nullptr)
            return;
        for (uint32_t paletteId = 0; paletteId < target.model->m_SkinPalettes.size(); ++paletteId)
            if (target.model->m_SkinPalettes[paletteId].numJoints > 0)
                target.palettes.push_back(paletteId);
        target.skeletons.resize(target.palettes.size());
        target.segmentHits.assign(target.palettes.size(), std::vector<SegmentHit>());
        target.sources.assign(target.palettes.size(), CutTargetSource());      // 원본 캐시는 첫 편집 프레임에 만든다 (등록 때는 아직 자세가 없다)
    }

    void CutController::RemoveJobsOf(CutTargetId id)
    {
        for (auto it = m_Jobs.begin(); it != m_Jobs.end();)
        {
            if (it->first.target == id)
            {
                it = m_Jobs.erase(it);
                ++m_JobListGeneration;
            }
            else
                ++it;
        }
    }

    const CutTarget* CutController::FindTarget(CutTargetId id) const
    {
        for (const CutTarget& target : m_Targets)
            if (target.id == id)
                return &target;
        return nullptr;
    }

    CutFrameStats CutController::GetFrameStats() const
    {
        CutFrameStats stats;
        stats.jobs = uint32_t(m_Jobs.size());
        stats.extractCount = m_ExtractCount;
        stats.pickMs = m_PickMs;
        for (const auto& entry : m_Jobs)
        {
            const CutJob& job = entry.second;
            stats.candidates += job.candidate ? 1 : 0;
            stats.confirmed += job.haveConfirmed ? 1 : 0;
            stats.extractMs += job.extractMs;
            stats.skinMs += job.skinMs;
            stats.previewMs += job.previewMs;
            stats.cutMs += job.cutMs;
        }
        return stats;
    }

    bool CutController::GetJointColors(CutTargetId target, uint32_t palette, std::vector<DirectX::XMFLOAT4>& colors) const
    {
        const CutTarget* found = FindTarget(target);
        if (found == nullptr || found->model == nullptr || palette >= found->model->m_SkinPalettes.size())
            return false;
        const uint32_t jointCount = found->model->m_SkinPalettes[palette].numJoints;
        colors.assign(jointCount, GetNoJobColor());
        std::vector<uint32_t> owner(jointCount, UINT32_MAX);        // 그 joint의 색을 정한 작업의 색 번호
        bool any = false;
        for (const auto& entry : m_Jobs)
        {
            const CutJob& job = entry.second;
            if (!job.candidate || job.key.target != target || job.key.kind != CutJobKind::SkeletalJoint || job.key.a != palette)
                continue;
            any = true;
            const uint32_t joint = job.key.b;
            if (joint < jointCount && (owner[joint] == UINT32_MAX || job.colorIndex < owner[joint]))
            {
                owner[joint] = job.colorIndex;
                colors[joint] = GetJobColor(job.colorIndex);
            }
        }
        return any;
    }

    void CutController::ToggleEditPlane()
    {
        if (!m_PlaneEditing && m_Targets.empty())
            return;
        m_PlaneEditing = !m_PlaneEditing;
        m_CutRequested = false;
        m_HavePickedFrame = false;      // 다음 Update에서 교차를 다시 찾는다
        for (CutTarget& target : m_Targets)
            for (CutTargetSource& cache : target.sources)
                cache.attempted = cache.attempted && cache.source != nullptr;      // 실패한 캐시는 다시 시도한다
        if (m_PlaneEditing)
        {
            if (!m_PlaneValid && m_Targets.front().instance != nullptr)
                PlacePlane(*m_Targets.front().instance);
            for (auto& entry : m_Jobs)
                entry.second.previewDirty = true;
        }
    }

    bool CutController::UpdateGizmo(const GizmoView& view, const GizmoInput& input)
    {
        if (!m_PlaneValid || !m_Plane.UpdateGizmo(view, input))
            return false;
        for (auto& entry : m_Jobs)
            entry.second.previewDirty = true;
        return true;
    }

    // 후보 작업이 하나도 없으면 요청은 버려진다.
    void CutController::RequestCut()
    {
        if (!m_PlaneEditing || !m_PlaneValid)
            return;
        for (const auto& entry : m_Jobs)
            if (entry.second.candidate && !entry.second.patch.Empty())
            {
                m_CutRequested = true;
                return;
            }
    }

    // 모든 확정 결과를 지운다. 확정 결과만 남아 있던(후보가 아닌) 작업은 삭제한다.
    void CutController::DiscardCut()
    {
        m_CutRequested = false;
        for (auto it = m_Jobs.begin(); it != m_Jobs.end();)
        {
            CutJob& job = it->second;
            job.haveConfirmed = false;
            job.cutAttempted = false;
            job.lastCutStatus = SliceStatus::InvalidInput;
            job.lastCutError.clear();
            job.cutInfo.clear();
            job.positive.Clear();
            job.negative.Clear();
            job.confirmedGeneration = ++m_Generation;
            if (!job.candidate)
            {
                it = m_Jobs.erase(it);
                ++m_JobListGeneration;
                continue;
            }
            job.previewDirty = true;
            ++it;
        }
    }

    // 모델이 바뀐 대상의 작업을 지우고, 평면을 편집 중이면 자세를 읽는다.
    void CutController::UpdateTargets()
    {
        for (CutTarget& target : m_Targets)
        {
            const Model* model = target.instance->GetModel();
            if (model != target.model)
            {
                RemoveJobsOf(target.id);
                target.model = model;
                ReloadPalettes(target);
            }
            if (model == nullptr || !m_PlaneEditing || !m_PlaneValid)
                continue;

            target.error.clear();
            for (size_t i = 0; i < target.palettes.size(); ++i)
            {
                std::string error;
                if (!ComputePaletteSkeleton(*model, *target.instance, target.palettes[i], target.skeletons[i], &error))
                {
                    target.skeletons[i] = SkeletonPose();
                    target.error = Utility::UTF8ToWideString(error);
                }
                if (!target.sources[i].attempted)
                {
                    target.sources[i].attempted = true;
                    BuildSource(target, i);
                }
            }
        }
    }

    // 대상의 팔레트 하나를 모든 joint로 추출해 원본 캐시로 둔다: 이 팔레트 스킨 메시의 정점(바인드 위치, joint, 가중치)과 삼각형 전체.
    void CutController::BuildSource(CutTarget& target, size_t slot)
    {
        CutTargetSource& cache = target.sources[slot];
        const uint32_t palette = target.palettes[slot];
        std::vector<uint32_t> joints(target.model->m_SkinPalettes[palette].numJoints);
        for (uint32_t joint = 0; joint < joints.size(); ++joint)
            joints[joint] = joint;

        ProceduralMeshData mesh;
        auto source = std::make_shared<BonePatchSource>();
        std::string error;
        if (!ExtractBonePatch(*target.model, *target.instance, palette, joints, 0.0f, mesh, *source, &error)
            || source->vertices.size() != mesh.vertices.size())
        {
            cache.error = Utility::UTF8ToWideString(error.empty() ? "palette source does not match" : error);
            return;
        }

        cache.pose.positions.resize(mesh.vertices.size());
        cache.pose.skins.resize(mesh.vertices.size());
        for (size_t i = 0; i < mesh.vertices.size(); ++i)
        {
            const BonePatchVertex& vertex = source->vertices[i];
            cache.pose.positions[i] = mesh.vertices[i].position;
            SkinVertex& skin = cache.pose.skins[i];
            skin.meshSlot = vertex.meshSlot;
            skin.bindPosition = vertex.position;
            skin.jointCount = 4;
            for (int k = 0; k < 4; ++k)
            {
                skin.joints[k] = vertex.joints[k];
                skin.weights[k] = vertex.weights[k];
            }
        }
        cache.pose.indices = mesh.indices;
        cache.source = source;
    }

    // 캐시 정점을 현재 자세로 옮기고(자세가 바뀐 때만) 평면과 교차하는 삼각형의 절단 joint를 다시 고른다(평면이나 자세가 바뀐 때만).
    void CutController::RefreshPick(CutTarget& target, size_t slot, bool planeChanged)
    {
        CutTargetSource& cache = target.sources[slot];
        std::vector<std::vector<DirectX::XMFLOAT4X4>> matrices;
        std::string error;
        if (!BuildSkinMatrices(*target.model, *target.instance, *cache.source, matrices, &error))
        {
            cache.error = Utility::UTF8ToWideString(error);
            cache.picked.clear();
            return;
        }
        cache.error.clear();
        const bool poseChanged = !SameMatrices(cache.matrices, matrices);
        if (!poseChanged && !planeChanged)
            return;

        const int64_t start = SystemTime::GetCurrentTick();
        if (poseChanged)
        {
            cache.matrices = std::move(matrices);
            for (size_t i = 0; i < cache.pose.positions.size(); ++i)
            {
                const SkinVertex& skin = cache.pose.skins[i];
                if (skin.meshSlot >= cache.matrices.size())
                    continue;
                const std::vector<DirectX::XMFLOAT4X4>& slotMatrices = cache.matrices[skin.meshSlot];
                DirectX::XMStoreFloat3(&cache.pose.positions[i], Skinning::SkinPoint(slotMatrices.data(), uint32_t(slotMatrices.size()),
                    skin.joints, skin.weights, 4, skin.bindPosition));
            }
        }
        CutPick::PickJoints(cache.pose, target.skeletons[slot].parents, m_Plane, kPickMinWeight, target.radius * kDefaultEpsilonScale, cache.picked);
        m_PickMs = ElapsedMs(start);
    }

    // 평면과 교차하는 삼각형이 고른 joint마다 작업을 만든다. 새 key만 추출하고, 사라진 key는 후보와 패치를 해제한다.
    // 평면이 지나는 뼈 선분은 작업과 무관하게 표시용으로만 찾는다.
    void CutController::UpdateCandidates()
    {
        const CutPlane::Frame frame = m_Plane.GetFrame();
        const bool planeChanged = !m_HavePickedFrame || memcmp(&frame, &m_PickedFrame, sizeof(frame)) != 0;

        std::vector<CutJobKey> current;
        std::map<CutJobKey, uint32_t> triangles;
        for (CutTarget& target : m_Targets)
        {
            for (size_t i = 0; i < target.palettes.size(); ++i)
            {
                m_Plane.PickSegments(target.skeletons[i], target.segmentHits[i]);
                if (target.sources[i].source == nullptr)
                    continue;
                RefreshPick(target, i, planeChanged);
                for (const CutPickJoint& picked : target.sources[i].picked)
                {
                    const CutJobKey key = MakeJointKey(target.id, target.palettes[i], picked.joint);
                    current.push_back(key);
                    triangles[key] = picked.triangleCount;
                }
            }
        }
        m_PickedFrame = frame;
        m_HavePickedFrame = true;
        ApplyDiff(DiffJobs(m_Jobs, current), triangles);
    }

    void CutController::ApplyDiff(const CutJobDiff& diff, const std::map<CutJobKey, uint32_t>& triangles)
    {
        for (const CutJobKey& key : diff.added)
        {
            const bool isNew = m_Jobs.find(key) == m_Jobs.end();
            CutJob& job = m_Jobs[key];
            if (isNew)
            {
                job.key = key;
                job.colorIndex = m_NextColorIndex++;
                ++m_JobListGeneration;
            }
            job.candidate = true;
            job.pickTriangles = triangles.at(key);
            ExtractJob(job);
        }
        for (const CutJobKey& key : diff.kept)
            m_Jobs.at(key).pickTriangles = triangles.at(key);
        for (const CutJobKey& key : diff.lost)
        {
            CutJob& job = m_Jobs.at(key);
            DropCandidate(job);
            if (!job.haveConfirmed)
            {
                m_Jobs.erase(key);
                ++m_JobListGeneration;
            }
        }
    }

    // 작업의 joint 하나(key.b)로 추출해 용접한다.
    void CutController::ExtractJob(CutJob& job)
    {
        job.extracted = true;
        job.previewDirty = true;
        ClearPreview(job, ++m_Generation);
        job.extractMs = 0.0f;

        const CutTarget* target = FindTarget(job.key.target);
        job.patch.Clear();
        job.weld = WeldStats();
        job.patchStatus = SliceStatus::InvalidInput;
        job.patchError.clear();
        job.patchTriangles = 0;
        if (target == nullptr || target->model == nullptr)
        {
            job.patchError = "no model";
            return;
        }

        const int64_t start = SystemTime::GetCurrentTick();
        ProceduralMeshData patch;
        auto source = std::make_shared<BonePatchSource>();
        WeldInput weldInput;
        std::string error;
        if (!ExtractBonePatch(*target->model, *target->instance, job.key.a, job.key.b, 0.0f, patch, *source, &error)
            || !MakeWeldInput(patch, *source, weldInput, &error))
            job.patchError = error;
        else
        {
            job.patchStatus = BuildPatchPiece(weldInput, kDefaultEpsilonScale, job.patch, job.weld, &job.patchError);
            if (job.patchStatus == SliceStatus::Ok)
            {
                job.patchRadius = float(ComputePieceRadius(job.patch));
                job.patchTriangles = uint32_t(patch.indices.size() / 3);
                job.source.bone = source;
                job.source.extractJoint = job.key.b;
            }
        }
        job.extractMs = ElapsedMs(start);
        ++m_ExtractCount;
    }

    // 후보가 사라진 작업: 패치와 미리보기를 해제한다. 확정 결과와 출처는 남긴다.
    void CutController::DropCandidate(CutJob& job)
    {
        job.candidate = false;
        job.extracted = false;
        job.patch.Clear();
        job.weld = WeldStats();
        job.patchStatus = SliceStatus::InvalidInput;
        job.patchError.clear();
        job.patchTriangles = 0;
        job.previousSkinMatrices.clear();
        job.poseValid = false;
        ClearPreview(job, ++m_Generation);
    }

    // 후보 패치와 확정 조각을 이번 자세로 옮긴다. 자세(스킨 행렬)가 바뀐 후보는 미리보기를 다시 계산하게 한다.
    void CutController::SkinJobs()
    {
        for (auto& entry : m_Jobs)
        {
            CutJob& job = entry.second;
            job.poseValid = false;
            job.skinError.clear();
            const bool havePatch = job.candidate && job.patchStatus == SliceStatus::Ok && !job.patch.Empty() && job.source.bone;
            if (!havePatch && !job.haveConfirmed)
                continue;
            const CutTarget* target = FindTarget(job.key.target);
            if (target == nullptr || target->model == nullptr || !job.source.bone)
            {
                job.skinError = "no source";
                continue;
            }

            std::string error;
            if (!BuildSkinMatrices(*target->model, *target->instance, *job.source.bone, job.skinMatrices, &error))
            {
                job.skinError = error;
                continue;
            }
            const int64_t start = SystemTime::GetCurrentTick();
            bool ok = true;
            if (havePatch)
            {
                ok = job.patch.skin && job.patch.skin->Apply(job.patch, job.skinMatrices, nullptr, &error);
                if (ok && !SameMatrices(job.previousSkinMatrices, job.skinMatrices))
                {
                    job.previousSkinMatrices = job.skinMatrices;
                    job.previewDirty = true;
                }
            }
            if (ok && job.haveConfirmed)
            {
                PatchPiece* pieces[2] = { &job.positive, &job.negative };
                for (PatchPiece* piece : pieces)
                    ok = ok && piece->skin && piece->skin->Apply(*piece, job.skinMatrices, nullptr, &error);
            }
            Smooth(job.skinMs, ElapsedMs(start));
            if (!ok)
            {
                job.skinError = error.empty() ? "piece has no skin" : error;
                continue;
            }
            job.poseValid = true;
        }
    }

    // 무한 평면으로 자른 뒤, 모든 루프 점이 유한 사각형 안에 있어야 받아들인다. 실패하면 status와 detail만 채운다.
    SliceStatus CutController::SliceWithPlane(const CutJob& job, SliceResult& result) const
    {
        SliceOptions options;
        options.cap = CapMode::Create;
        const SliceStatus status = SlicePatch(job.patch, m_Plane.ToSlicePlane(), 1, options, result);
        if (status != SliceStatus::Ok)
            return status;

        const CutTarget* target = FindTarget(job.key.target);
        const float margin = (target != nullptr ? target->radius : m_ModelRadius) * 0.01f;
        std::string detail;
        if (!m_Plane.ContainsLoops(result.positive, result.loops, margin, &detail))
        {
            result = SliceResult();
            result.status = SliceStatus::LoopOutsidePlane;
            result.detail = detail;
            return SliceStatus::LoopOutsidePlane;
        }
        return SliceStatus::Ok;
    }

    // 현재 평면으로 미절단 패치를 시험 절단한다. 결과는 보여 주기만 하고 확정 조각은 건드리지 않는다.
    void CutController::RunPreview(CutJob& job)
    {
        job.previewDirty = false;
        const int64_t start = SystemTime::GetCurrentTick();
        job.previewStatus = SliceWithPlane(job, job.preview);
        Smooth(job.previewMs, ElapsedMs(start));
        job.havePreview = true;
        job.previewGeneration = ++m_Generation;
    }

    // 먼저 이전 확정 결과를 모두 지우고(Ctrl+Z와 같다), 후보 작업마다 독립으로 자른다. 실패한 작업은 결과가 없다.
    void CutController::ConfirmAll()
    {
        DiscardCut();
        for (auto& entry : m_Jobs)
        {
            CutJob& job = entry.second;
            if (!job.candidate)
                continue;
            job.cutAttempted = true;
            job.lastCutError.clear();
            job.cutInfo.clear();
            if (job.patchStatus != SliceStatus::Ok || !job.poseValid)
            {
                job.lastCutStatus = job.patchStatus != SliceStatus::Ok ? job.patchStatus : SliceStatus::InvalidInput;
                job.lastCutError = job.patchStatus != SliceStatus::Ok ? job.patchError : job.skinError;
                continue;
            }

            SliceResult result;
            const int64_t start = SystemTime::GetCurrentTick();
            job.lastCutStatus = SliceWithPlane(job, result);
            job.cutMs = ElapsedMs(start);
            job.lastCutError = result.detail;
            if (job.lastCutStatus != SliceStatus::Ok)
                continue;

            job.confirmedNormal = m_Plane.ToSlicePlane().normal;
            job.positive = result.positive;
            job.negative = result.negative;
            job.haveConfirmed = true;
            job.confirmedGeneration = ++m_Generation;
            ClearPreview(job, ++m_Generation);
            job.previewDirty = false;

            const double original = TotalArea(job.patch, PatchFaceKind::Surface);
            const double positive = TotalArea(result.positive, PatchFaceKind::Surface);
            const double negative = TotalArea(result.negative, PatchFaceKind::Surface);
            wchar_t info[200];
            swprintf_s(info, L"loops %zu  + %zu faces  - %zu faces  area diff %.1e", result.loops.size(),
                result.positive.faces.size(), result.negative.faces.size(), positive + negative - original);
            job.cutInfo = info;
        }
    }

    void CutController::Update()
    {
        ScopedTimer _prof(L"Patch Pose Update");
        UpdateTargets();
        if (m_PlaneEditing && m_PlaneValid)
            UpdateCandidates();
        else
            ApplyDiff(DiffJobs(m_Jobs, {}), {});
        SkinJobs();

        if (m_CutRequested)
        {
            m_CutRequested = false;
            if (m_PlaneValid)
                ConfirmAll();
        }
        else if (m_PlaneEditing && m_PlaneValid)
        {
            for (auto& entry : m_Jobs)
            {
                CutJob& job = entry.second;
                if (job.candidate && job.previewDirty && job.patchStatus == SliceStatus::Ok && job.poseValid)
                    RunPreview(job);
            }
        }
    }

    // 모델 bounds 중심에 수평 평면을 놓는다 (반 크기 = 반지름 / 2).
    void CutController::PlacePlane(const ModelInstance& instance)
    {
        const Vector3 center = instance.GetCenter();
        m_ModelRadius = float(instance.GetRadius());
        if (!(m_ModelRadius > 0.0f))
            m_ModelRadius = 1.0f;

        const GizmoTool tool = m_Plane.GetGizmoTool();      // 사용자가 고른 도구는 유지한다
        m_Plane = CutPlane::MakeHorizontal({ float(center.GetX()), float(center.GetY()), float(center.GetZ()) }, m_ModelRadius);
        m_Plane.SetGizmoTool(tool);
        m_PlaneValid = true;
    }
}
