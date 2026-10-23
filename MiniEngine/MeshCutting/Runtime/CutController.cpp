#include "../pch.h"
#include "CutController.h"
#include "../BonePatch.h"
#include "../StaticMeshCopy.h"
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
        const float kBreakOffsetRatio = 0.3f;      // 끊긴 joint를 평면 법선으로 옮기는 거리 / 대상 반지름 (Unreal Impulse 대신)

        // 지수 평균. 값이 0이면 첫 표본으로 시작한다.
        void Smooth(float& value, float sample)
        {
            value = value == 0.0f ? sample : value + (sample - value) * 0.1f;
        }

        // 모델의 메시 하나의 정점 수 (VB 크기 / stride). 범위 밖이면 0.
        uint32_t MeshVertexCount(const Model& model, uint32_t meshIndex)
        {
            const uint8_t* meshPtr = model.m_MeshData.get();
            if (meshPtr == nullptr || meshIndex >= model.m_NumMeshes)
                return 0;
            for (uint32_t i = 0; i < meshIndex; ++i)
                meshPtr += sizeof(Mesh) + (((const Mesh*)meshPtr)->numDraws - 1) * sizeof(Mesh::Draw);
            const Mesh& mesh = *(const Mesh*)meshPtr;
            return mesh.vbStride == 0 ? 0 : mesh.vbSize / mesh.vbStride;
        }

        // 끊을 노드의 자손 joint(없으면 자기 자신)가 평면 법선 쪽에 있으면 +1, 아니면 -1. 절단 때 자세(poseJointWorld) 기준.
        float ChildSide(const Model& model, const MeshSource& source, uint32_t breakNode, const SlicePlane& plane)
        {
            const SkinPalette& palette = model.m_SkinPalettes[source.paletteId];
            const Vector3 normal(plane.normal.x, plane.normal.y, plane.normal.z);
            const Vector3 point(plane.point.x, plane.point.y, plane.point.z);
            float childSum = 0.0f, selfSum = 0.0f;
            bool anyChild = false;
            for (uint32_t j = 0; j < palette.numJoints && j < source.poseJointWorld.size(); ++j)
            {
                uint32_t node = model.m_JointIndices[palette.startJoint + j];
                const float distance = float(Dot(normal, Vector3(source.poseJointWorld[j].GetW()) - point));
                if (node == breakNode)
                    selfSum = distance;
                for (uint32_t depth = 0; node != breakNode && depth <= model.m_NumNodes && node < model.m_NodeParents.size(); ++depth)
                {
                    node = model.m_NodeParents[node];
                    if (node == breakNode)
                    {
                        childSum += distance;
                        anyChild = true;
                    }
                }
            }
            return (anyChild ? childSum : selfSum) >= 0.0f ? 1.0f : -1.0f;
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

        // 한 단계의 시간을 재서 phase.frame에 더한다. 프로파일러 범위(ScopedTimer)도 함께 열고, 삼각형이나 정점마다 부르지 않는다.
        // ScopedTimer는 Release에서 비어 있어서, 화면에 보이는 값은 직접 잰 시간이다.
        class PhaseTimer
        {
        public:
            PhaseTimer(const wchar_t* name, CutPhaseTime& phase) : m_Profile(name), m_Phase(phase), m_Start(SystemTime::GetCurrentTick()) {}
            ~PhaseTimer() { m_Phase.frame += ElapsedMs(m_Start); }
            PhaseTimer(const PhaseTimer&) = delete;
            PhaseTimer& operator=(const PhaseTimer&) = delete;
        private:
            ScopedTimer m_Profile;
            CutPhaseTime& m_Phase;
            int64_t m_Start;
        };

        // 프레임 값으로 지수 평균을 갱신한다 (건너뛴 프레임의 0도 평균에 들어간다).
        void UpdateAverage(CutPhaseTime& phase)
        {
            phase.average = phase.average == 0.0f ? phase.frame : phase.average + (phase.frame - phase.average) * 0.1f;
        }

        // 스킨 없는 메시의 현재 world 행렬 (BuildRigidMatrices와 같은 값). 자세가 없으면 false.
        bool GetMeshWorld(const CutTarget& target, uint16_t meshCBV, DirectX::XMFLOAT4X4& world)
        {
            const Matrix4* nodeWorld = target.instance->GetNodeWorldMatrices();
            if (nodeWorld == nullptr || target.model == nullptr || meshCBV >= target.model->m_NumNodes)
                return false;
            DirectX::XMStoreFloat4x4(&world, DirectX::XMMATRIX(nodeWorld[meshCBV]));
            return true;
        }

        // 작업의 위치 갱신 행렬: 스킨 없는 메시는 world 행렬 하나(강체), 스켈레탈은 joint 행렬
        bool BuildJobMatrices(const CutTarget& target, const CutJob& job, std::vector<std::vector<DirectX::XMFLOAT4X4>>& out, std::string* error)
        {
            if (job.key.kind == CutJobKind::StaticMesh)
                return BuildRigidMatrices(*target.model, *target.instance, *job.source, out, error);
            return BuildSkinMatrices(*target.model, *target.instance, *job.source, out, error);
        }

        // 끊긴 작업의 조각 하나의 행렬: 지금 행렬에서 한쪽 joint를 절단 순간 값에 강체 이동(지금 world * inverse(절단 world))을 곱한 것으로 바꾼다.
        // 떨어지는 조각은 끊기지 않은 joint를 끊긴 노드 world로, 남는 조각은 끊긴 joint를 부모 world로 따라간다.
        bool BuildPieceMatrices(const CutTarget& target, const CutJob& job, bool positive, std::vector<std::vector<DirectX::XMFLOAT4X4>>& out, std::string* error)
        {
            if (!BuildJobMatrices(target, job, out, error))
                return false;
            if (job.breakNode == kInvalidIndex)
                return true;
            const bool falling = positive == job.positiveFalls;
            const Matrix4* nodeWorld = target.instance->GetNodeWorldMatrices();
            Matrix4 follow(kIdentity);      // 부모가 없는 남는 조각은 절단 순간 자리에 그대로 둔다
            if (falling || job.breakHasParent)
            {
                const uint32_t followNode = falling ? job.breakNode : job.breakParent;
                const DirectX::XMFLOAT4X4& followCut = falling ? job.breakNodeWorld : job.breakParentWorld;
                follow = nodeWorld[followNode] * Invert(Matrix4(DirectX::XMLoadFloat4x4(&followCut)));
            }
            const MeshSource& source = *job.source;
            for (size_t slot = 0; slot < out.size() && slot < job.breakCutMatrices.size(); ++slot)
            {
                const Matrix4 meshFollow = nodeWorld[source.meshes[slot].worldMatrixIndex] * follow;
                for (size_t j = 0; j < out[slot].size() && j < job.breakDetached.size(); ++j)
                    if ((job.breakDetached[j] != 0) != falling)
                        DirectX::XMStoreFloat4x4(&out[slot][j], DirectX::XMMATRIX(meshFollow * Matrix4(DirectX::XMLoadFloat4x4(&job.breakCutMatrices[slot][j]))));
            }
            return true;
        }

    }

    // 모델을 식별 가능한 대상으로 등록(게임 오브젝트화)
    CutTargetId CutController::RegisterInstance(ModelInstance& instance, std::wstring name)
    {
        CutTarget target;
        target.id = m_NextTargetId++;
        target.instance = &instance;
        target.model = instance.GetModel();
        target.name = std::move(name);
        InitializeTargetModelData(target);
        m_Targets.push_back(std::move(target));
        return m_Targets.back().id;
    }

    // 모델 등록 해제
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

    void CutController::InitializeTargetModelData(CutTarget& target)
    {
        // 초기화
        target.palettes.clear();
        target.skeletons.clear();
        target.staticMeshes.clear();
        target.staticMeshesInitialized = false;
        target.error.clear();
        target.radius = float(target.instance->GetRadius());
        if (!(target.radius > 0.0f))
            target.radius = 1.0f;
        if (target.model == nullptr)
            return;

        // 미니엔진에서 추출해둔 skin palettes 데이터 연결
        for (uint32_t paletteId = 0; paletteId < target.model->m_SkinPalettes.size(); ++paletteId)
            if (target.model->m_SkinPalettes[paletteId].numJoints > 0)
                target.palettes.push_back(paletteId);

        // 팔레트 개수를 참고하여 메모리 공간 할당
        target.skeletons.resize(target.palettes.size());
        target.sources.assign(target.palettes.size(), CutTargetSource()); // 원본 캐시는 첫 편집 프레임에 만든다 (등록 때는 아직 자세가 없다)
        target.segmentHits.assign(target.palettes.size(), std::vector<SegmentHit>()); // 디버깅용 본(선분)
    }

    // 타겟의 절단 작업을 되돌린다.
    void CutController::RemoveJobsOf(CutTargetId id)
    {
        for (auto it = m_Jobs.begin(); it != m_Jobs.end();)
        {
            if (it->first.target == id)
            {
                if (it->second.haveConfirmed)
                    SetOriginalVisible(it->second, true);     // 확정으로 숨긴 것만 되돌린다
                it = m_Jobs.erase(it);
                ++m_JobListGeneration;
            }
            else
                ++it;
        }
        for (CutTarget& target : m_Targets)
        {
            if (target.id == id)
            {
                UpdateSkinMask(target);
                UpdateBrokenJoints(target);
            }
        }
    }

    // 확정된 스켈레탈 작업의 joint를 BreakConstraint로 끊는다. 끊은 노드 집합이 달라졌을 때만 모두 되돌리고 다시 끊는다.
    void CutController::UpdateBrokenJoints(CutTarget& target)
    {
        std::map<uint32_t, Vector3> wanted;
        for (const auto& entry : m_Jobs)
        {
            const CutJob& job = entry.second;
            if (job.key.target == target.id && job.key.kind == CutJobKind::SkeletalJoint && job.haveConfirmed && job.breakNode != kInvalidIndex)
                wanted.emplace(job.breakNode, Vector3(job.breakOffsetNode.x, job.breakOffsetNode.y, job.breakOffsetNode.z));
        }
        bool same = wanted.size() == target.brokenNodes.size();
        for (auto it = wanted.begin(); same && it != wanted.end(); ++it)
            same = target.brokenNodes.count(it->first) != 0;
        if (same)
            return;
        target.instance->ResetBrokenConstraints();
        target.brokenNodes.clear();
        for (const auto& entry : wanted)
        {
            target.instance->BreakConstraint(entry.second, entry.first);
            target.brokenNodes.insert(entry.first);
        }
    }

    // 확정된 스켈레탈 joint 작업의 끊을 노드와 이동량을 정한다: 평면 법선 방향 x 반지름 x 비율, 부호는 자손 쪽 (끊긴 쪽이 평면에서 멀어진다).
    void CutController::SetBreakOf(CutJob& job)
    {
        job.breakNode = kInvalidIndex;
        const CutTarget* target = FindTarget(job.key.target);
        if (job.key.kind != CutJobKind::SkeletalJoint || !job.haveConfirmed || !job.source || target == nullptr || target->model == nullptr)
            return;
        const MeshSource& source = *job.source;
        const Model& model = *target->model;
        if (source.paletteId >= model.m_SkinPalettes.size())
            return;
        const SkinPalette& palette = model.m_SkinPalettes[source.paletteId];
        if (job.key.b >= palette.numJoints)
            return;
        const uint32_t node = model.m_JointIndices[palette.startJoint + job.key.b];     // 노드 번호 = matrixIdx
        if (node >= model.m_NumNodes)
            return;
        const SlicePlane plane = m_Plane.ToSlicePlane();
        const float distance = ChildSide(model, source, node, plane) * target->radius * kBreakOffsetRatio;
        job.breakNode = node;
        job.breakOffset = { plane.normal.x * distance, plane.normal.y * distance, plane.normal.z * distance };

        // 절단 순간 값 저장: 끊기 전 자세의 slot 행렬(메시 world는 뺀다), 끊긴 노드와 부모의 world, 끊긴 joint 집합
        const Matrix4* nodeWorld = target->instance->GetNodeWorldMatrices();
        if (nodeWorld == nullptr || job.skinMatrices.size() != source.meshes.size())
        {
            job.breakNode = kInvalidIndex;
            return;
        }
        // world 이동량을 노드 행렬 공간(메시 world를 뺀 공간)으로 바꾼다
        const Vector3 nodeOffset = Invert(nodeWorld[source.meshes[0].worldMatrixIndex]).Get3x3() * Vector3(job.breakOffset.x, job.breakOffset.y, job.breakOffset.z);
        DirectX::XMStoreFloat3(&job.breakOffsetNode, DirectX::XMVECTOR(nodeOffset));
        job.breakCutMatrices = job.skinMatrices;
        for (size_t slot = 0; slot < source.meshes.size(); ++slot)
        {
            const Matrix4 inverseMeshWorld = Invert(nodeWorld[source.meshes[slot].worldMatrixIndex]);
            for (DirectX::XMFLOAT4X4& matrix : job.breakCutMatrices[slot])
                DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMATRIX(inverseMeshWorld * Matrix4(DirectX::XMLoadFloat4x4(&matrix))));
        }
        DirectX::XMStoreFloat4x4(&job.breakNodeWorld, DirectX::XMMATRIX(nodeWorld[node]));
        job.breakParent = model.m_NodeParents[node];
        job.breakHasParent = job.breakParent < model.m_NumNodes;
        if (job.breakHasParent)
            DirectX::XMStoreFloat4x4(&job.breakParentWorld, DirectX::XMMATRIX(nodeWorld[job.breakParent]));
        job.breakDetached.assign(palette.numJoints, 0);
        for (uint32_t j = 0; j < palette.numJoints; ++j)
        {
            uint32_t ancestor = model.m_JointIndices[palette.startJoint + j];
            for (uint32_t depth = 0; depth <= model.m_NumNodes && ancestor < model.m_NodeParents.size(); ++depth)
            {
                if (ancestor == node)
                {
                    job.breakDetached[j] = 1;
                    break;
                }
                ancestor = model.m_NodeParents[ancestor];
            }
        }
        job.positiveFalls = job.breakOffset.x * plane.normal.x + job.breakOffset.y * plane.normal.y + job.breakOffset.z * plane.normal.z > 0.0f;
    }

    // 확정된 스켈레탈 작업이 복사한 joint 영향 범위를 메시별 버텍스 컬러(R=255)로 덮어써 원본에서 숨긴다 (Unreal SetVertexColorOverride).
    // 대상의 확정 작업을 처음부터 다시 모으므로 (대상, 메시)별 OR가 되고, 더는 확정이 없는 메시는 덮어쓴 색을 지운다.
    void CutController::UpdateSkinMask(CutTarget& target)
    {
        std::map<uint32_t, std::vector<uint32_t>> masks;
        for (const auto& entry : m_Jobs)
        {
            const CutJob& job = entry.second;
            if (job.key.target != target.id || job.key.kind != CutJobKind::SkeletalJoint || !job.haveConfirmed || job.source == nullptr)
                continue;
            const MeshSource& source = *job.source;
            for (const SourceVertex& vertex : source.vertices)
            {
                if (vertex.meshSlot >= source.meshes.size()
                    || !vertex.selected)
                    continue;
                const uint32_t meshIndex = source.meshes[vertex.meshSlot].meshIndex;
                std::vector<uint32_t>& colors = masks[meshIndex];
                if (colors.empty())
                    colors.assign(MeshVertexCount(*target.model, meshIndex), 0u);
                if (vertex.sourceVertex < colors.size())
                    colors[vertex.sourceVertex] = 0x000000FFu;     // RGBA8의 R 채널이 마스크
            }
        }
        for (uint32_t meshIndex : target.maskedMeshes)
            if (masks.find(meshIndex) == masks.end())
                target.instance->ClearVertexColorOverride(meshIndex);
        target.maskedMeshes.clear();
        for (const auto& mask : masks)
        {
            if (target.instance->SetVertexColorOverride(mask.first, mask.second))
                target.maskedMeshes.insert(mask.first);
            else
                target.instance->ClearVertexColorOverride(mask.first);
        }
    }

    // 스태틱 메시 작업의 원본 메시를 보이거나 숨긴다 (Unreal SetVisibility). 스켈레탈 작업은 건드리지 않는다.
    void CutController::SetOriginalVisible(const CutJob& job, bool visible)
    {
        if (job.key.kind != CutJobKind::StaticMesh)
            return;
        for (CutTarget& target : m_Targets)
            if (target.id == job.key.target)
                target.instance->SetMeshVisibility(job.key.a, visible);
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
        CutFrameStats stats = m_Frame;
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
        if (m_PlaneValid)
            m_PlaneValid = false;
        else
        {
            if (m_Targets.empty() || m_Targets.front().instance == nullptr)
                return;
            PlacePlane(*m_Targets.front().instance);
        }
        m_CutRequested = false;
        m_HavePickedFrame = false;      // 다음 Update에서 교차를 다시 찾는다
        for (CutTarget& target : m_Targets)
        {
            for (CutTargetSource& cache : target.sources)
                cache.attempted = cache.attempted && cache.source != nullptr;      // 실패한 캐시는 다시 시도한다
        }
    }

    bool CutController::UpdateGizmo(const GizmoView& view, const GizmoInput& input)
    {
        return m_PlaneValid && m_Plane.UpdateGizmo(view, input);
    }

    // 후보 작업이 하나도 없으면 요청은 버려진다.
    void CutController::RequestCut()
    {
        if (!m_PlaneValid)
            return;
        for (const auto& entry : m_Jobs)
            if (entry.second.candidate)
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
            if (job.haveConfirmed)
                SetOriginalVisible(job, true);
            job.haveConfirmed = false;
            job.breakNode = kInvalidIndex;
            job.cutAttempted = false;
            job.lastCutStatus = SliceStatus::InvalidInput;
            job.lastCutError.clear();
            job.cutInfo.clear();
            job.positive.reset();
            job.negative.reset();
            job.appliedMatrices[0].clear();
            job.appliedMatrices[1].clear();
            job.confirmedGeneration = ++m_Generation;
            if (!job.candidate)
            {
                it = m_Jobs.erase(it);
                ++m_JobListGeneration;
                continue;
            }
            ++it;
        }
        for (CutTarget& target : m_Targets)
        {
            UpdateSkinMask(target);
            UpdateBrokenJoints(target);
        }
    }

    // 평면 활성화되어 있으면 미니엔진에서 계산한 본 행렬을 참조해
    // 본 위치와 계층 정보를 갱신하고, 필요한 원본 메시 캐시를 준비한다.
    // (미니엔진에서는 스키닝 정보가 GPU에서 바로 계산되기 때문에 CPU에서 이를 가져올 방법이 없다.)
    void CutController::UpdateTarget(CutTarget& target)
    {
        const Model* model = target.instance != nullptr ? target.instance->GetModel() : nullptr;

        // 평면 없으면 건너뛰기
        if (model == nullptr || !m_PlaneValid)
            return;

        // target의 스켈레탈 메시 정보를 업데이트 한다.
        target.error.clear();
        for (size_t i = 0; i < target.palettes.size(); ++i)
        {
            std::string error;
            bool skeletonOk;
            {
                // 미니 엔진의 노드 행렬을 참고하여 현재 본의 위치, 부모 관계를 계산한다.
                PhaseTimer timer(L"Palette skeleton", m_Frame.skeleton);
                skeletonOk = ComputePaletteSkeleton(*model, *target.instance, target.palettes[i], target.skeletons[i], &error);
            }

            // 실패 처리
            if (!skeletonOk)
            {
                target.skeletons[i] = SkeletonPose();
                target.error = Utility::UTF8ToWideString(error);
            }

            // 원본 캐시 생성
            if (!target.sources[i].attempted)
            {
                target.sources[i].attempted = true;
                BuildSource(target, i);
            }
        }
    }

    // 정적 메시 목록과 교차 검사에 쓸 로컬 정점·인덱스 캐시를 함께 만든다.
    // ReadStaticMesh가 노드 행렬을 요구하므로 Update에서 자세 갱신 뒤 최초 한 번 호출한다.
    void CutController::InitCutStaticMesh(CutTarget& target)
    {
        if (target.staticMeshesInitialized || target.model == nullptr || target.instance == nullptr)
            return;

        // 빈 목록도 완료로 기록해 매 프레임 목록을 다시 만들지 않는다.
        target.staticMeshesInitialized = true;
        std::vector<StaticMeshInfo> infos;
        GetModelStaticMeshes(*target.model, infos);
        target.staticMeshes.reserve(infos.size());
        for (const StaticMeshInfo& info : infos)
        {
            CutStaticMesh mesh;
            mesh.meshIndex = info.meshIndex;
            mesh.meshCBV = info.meshCBV;
            mesh.bounds = info.bounds;
            UpdateStaticMeshCache(target, mesh);
            target.staticMeshes.push_back(std::move(mesh));
        }
    }

    // Static Mesh 하나를 복사해(CopyProceduralMeshFromStaticMesh) 로컬 위치와 인덱스만 pick용 캐시로 둔다.
    void CutController::UpdateStaticMeshCache(const CutTarget& target, CutStaticMesh& mesh)
    {
        ProceduralMeshData copy;
        MeshSource source;
        std::string error;
        mesh.positions.clear();
        mesh.indices.clear();
        mesh.havePicked = false;
        if (!ReadStaticMesh(*target.model, *target.instance, mesh.meshIndex, copy, source, &error))
        {
            mesh.error = Utility::UTF8ToWideString(error);
            return;
        }
        mesh.error.clear();
        mesh.positions.resize(source.vertices.size());
        for (size_t i = 0; i < source.vertices.size(); ++i)
            mesh.positions[i] = copy.bindings[i].bindPosition;
        mesh.indices = std::move(copy.indices);
    }

    // 스킨 없는 메시의 후보 찾기 (Unreal 섹션 박스 조기 판정과 같은 방식): 바운딩 구가 평면 사각형과 안 만나면 건너뛰고,
    // 만나면 평면을 로컬로 옮겨 삼각형을 본다. 평면이나 메시 world 행렬이 바뀐 프레임에만 다시 계산한다.
    void CutController::PickStaticMeshes(CutTarget& target, bool planeChanged, std::vector<CutJobKey>& current, std::map<CutJobKey, uint32_t>& triangles)
    {
        const float epsilon = target.radius * kDefaultEpsilonScale;
        for (CutStaticMesh& mesh : target.staticMeshes)
        {
            ++m_Frame.staticMeshes;
            DirectX::XMFLOAT4X4 world;
            if (mesh.positions.empty() || !GetMeshWorld(target, mesh.meshCBV, world))
                continue;
            if (planeChanged || !mesh.havePicked || memcmp(&world, &mesh.world, sizeof(world)) != 0)
            {
                PhaseTimer timer(L"Pick static", m_Frame.pick);
                mesh.world = world;
                mesh.havePicked = true;
                mesh.crossed = 0;
                if (!CutPick::SphereMayCross(mesh.bounds, world, m_Plane, epsilon))
                    ++m_Frame.staticCulled;
                else
                {
                    CutPickStats pickStats;
                    mesh.crossed = CutPick::CountCrossedTriangles(mesh.positions, mesh.indices, world, m_Plane, epsilon, &pickStats);
                    m_Frame.staticTriangles += pickStats.checkedTriangles;
                    m_Frame.mixedTriangles += pickStats.mixedTriangles;
                    m_Frame.crossedTriangles += pickStats.crossedTriangles;
                }
            }
            if (mesh.crossed > 0)
            {
                const CutJobKey key = MakeStaticMeshKey(target.id, mesh.meshIndex);
                current.push_back(key);
                triangles[key] = mesh.crossed;
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
        auto source = std::make_shared<MeshSource>();
        std::string error;
        if (!ReadBonePatch(*target.model, *target.instance, palette, joints, 0.0f, mesh, *source, &error)
            || source->vertices.size() != mesh.vertices.size())
        {
            cache.error = Utility::UTF8ToWideString(error.empty() ? "palette source does not match" : error);
            return;
        }

        cache.pose.positions.resize(mesh.vertices.size());
        cache.pose.skins.resize(mesh.vertices.size());
        for (size_t i = 0; i < mesh.vertices.size(); ++i)
        {
            const MeshVertexBinding& vertex = mesh.bindings[i];
            cache.pose.positions[i] = mesh.vertices[i].position;
            SkinVertex& skin = cache.pose.skins[i];
            skin.meshSlot = vertex.meshSlot;
            skin.bindPosition = vertex.bindPosition;
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
        bool poseChanged;
        {
            PhaseTimer timer(L"Pick matrices", m_Frame.poseMatrices);
            if (!BuildSkinMatrices(*target.model, *target.instance, *cache.source, matrices, &error))
            {
                cache.error = Utility::UTF8ToWideString(error);
                cache.picked.clear();
                return;
            }
            cache.error.clear();
            poseChanged = !SameMatrices(cache.matrices, matrices);
        }
        if (!poseChanged && !planeChanged)
        {
            ++m_Frame.refreshSkipped;
            return;
        }
        if (poseChanged && planeChanged)
            ++m_Frame.refreshBoth;
        else if (poseChanged)
            ++m_Frame.refreshPose;
        else
            ++m_Frame.refreshPlane;

        const int64_t start = SystemTime::GetCurrentTick();
        if (poseChanged)
        {
            PhaseTimer timer(L"Pick skinning", m_Frame.cacheSkin);
            m_Frame.cacheVertices += uint32_t(cache.pose.positions.size());
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
        {
            PhaseTimer timer(L"Pick triangles", m_Frame.pick);
            CutPickStats pickStats;
            CutPick::PickJoints(cache.pose, target.skeletons[slot].parents, m_Plane, kPickMinWeight, target.radius * kDefaultEpsilonScale, cache.picked, &pickStats);
            m_Frame.checkedTriangles += pickStats.checkedTriangles;
            m_Frame.mixedTriangles += pickStats.mixedTriangles;
            m_Frame.crossedTriangles += pickStats.crossedTriangles;
        }
        m_PickMs = ElapsedMs(start);
    }

    // 평면과 교차하는 삼각형이 고른 joint마다 작업 key를 만든다. 패치는 만들지 않고(Enter 때 만든다), 사라진 key는 후보에서 뺀다.
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
                {
                    PhaseTimer timer(L"Pick segments", m_Frame.segments);
                    m_Plane.PickSegments(target.skeletons[i], target.segmentHits[i]);
                }
                if (target.sources[i].source == nullptr)
                    continue;
                RefreshPick(target, i, planeChanged);
                PhaseTimer timer(L"Job update", m_Frame.jobUpdate);
                for (const CutPickJoint& picked : target.sources[i].picked)
                {
                    const CutJobKey key = MakeJointKey(target.id, target.palettes[i], picked.joint);
                    current.push_back(key);
                    triangles[key] = picked.triangleCount;
                }
            }
            PickStaticMeshes(target, planeChanged, current, triangles);
        }
        m_PickedFrame = frame;
        m_HavePickedFrame = true;
        PhaseTimer timer(L"Job update", m_Frame.jobUpdate);
        ApplyDiff(DiffJobs(m_Jobs, current), triangles);
    }

    void CutController::ApplyDiff(const CutJobDiff& diff, const std::map<CutJobKey, uint32_t>& triangles)
    {
        m_Frame.jobsAdded += uint32_t(diff.added.size());
        m_Frame.jobsKept += uint32_t(diff.kept.size());
        m_Frame.jobsLost += uint32_t(diff.lost.size());
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

    // 작업을 추출해 용접한다 (스켈레탈: joint 하나(key.b), 스태틱: 메시 하나를 복사). Enter 때만 부른다.
    void CutController::ExtractJob(CutJob& job, ProceduralMesh& mesh)
    {
        job.extractMs = 0.0f;
        job.weld = WeldStats();
        job.patchStatus = SliceStatus::InvalidInput;
        job.patchError.clear();
        job.patchTriangles = 0;
        const CutTarget* target = FindTarget(job.key.target);
        if (target == nullptr || target->model == nullptr)
        {
            job.patchError = "no model";
            return;
        }
        PhaseTimer timer(L"Extract job", m_Frame.extract);
        ++m_Frame.extractions;
        const int64_t start = SystemTime::GetCurrentTick();
        auto source = std::make_shared<MeshSource>();
        job.patchStatus = job.key.kind == CutJobKind::StaticMesh
            ? CopyProceduralMeshFromStaticMesh(*target->model, *target->instance, job.key.a,
                mesh, *source, job.weld, &job.patchError)
            : ExtractBonePatch(*target->model, *target->instance, job.key.a, job.key.b, 0.0f,
                mesh, *source, job.weld, &job.patchError);
        if (job.patchStatus == SliceStatus::Ok)
        {
            job.patchTriangles = uint32_t(mesh.GetGeometry().faces.size());
            job.source = std::move(source);
        }
        job.extractMs = ElapsedMs(start);
        ++m_ExtractCount;
    }

    // 후보가 사라진 작업: 후보 표시만 끈다. 확정 결과와 출처는 남긴다.
    void CutController::DropCandidate(CutJob& job)
    {
        job.candidate = false;
        job.pickTriangles = 0;
    }

    // 확정 조각을 이번 자세로 옮긴다. 후보 작업은 패치가 없으므로 옮기지 않는다.
    void CutController::SkinJobs()
    {
        for (auto& entry : m_Jobs)
        {
            CutJob& job = entry.second;
            job.poseValid = false;
            job.skinError.clear();
            if (!job.haveConfirmed)
                continue;
            const CutTarget* target = FindTarget(job.key.target);
            if (target == nullptr || target->model == nullptr || !job.source)
            {
                job.skinError = "no source";
                continue;
            }

            std::string error;
            if (job.key.kind == CutJobKind::StaticMesh)
            {
                // 스태틱 조각: 정점은 절단 순간 world에 고정돼 있다. 지금 메시 world와의 차이(지금 × inverse(절단 순간))만 구해
                // 그릴 때 곱한다. 정점을 다시 쓰지 않으므로 렌더러도 다시 올리지 않는다.
                PhaseTimer timer(L"Job matrices", m_Frame.jobMatrices);
                if (!BuildJobMatrices(*target, job, job.skinMatrices, &error) || job.skinMatrices.empty() || job.skinMatrices[0].empty())
                {
                    job.skinError = error.empty() ? "no mesh world" : error;
                    continue;
                }
                const Matrix4 now(DirectX::XMLoadFloat4x4(&job.skinMatrices[0][0]));
                const Matrix4 atCut(DirectX::XMLoadFloat4x4(&job.cutWorld));
                DirectX::XMStoreFloat4x4(&job.pieceWorld, DirectX::XMMATRIX(now * Invert(atCut)));
                m_Frame.jobSkipped += 2;
                job.poseValid = true;
                continue;
            }

            bool matricesOk = true;
            bool ok = true;
            const int64_t start = SystemTime::GetCurrentTick();
            ProceduralMesh* pieces[2] = { job.positive.get(), job.negative.get() };
            for (int side = 0; side < 2 && matricesOk && ok; ++side)
            {
                {
                    PhaseTimer timer(L"Job matrices", m_Frame.jobMatrices);
                    matricesOk = BuildPieceMatrices(*target, job, side == 0, job.skinMatrices, &error);     // 끊긴 작업은 조각마다 행렬이 다르다
                }
                if (!matricesOk)
                    break;
                // 행렬이 지난 적용 때와 같으면 갱신하지 않는다. 버전이 오르지 않아 렌더러도 다시 올리지 않는다.
                // 끊긴 joint 작업도 BuildPieceMatrices가 조각의 행렬을 완성해 주므로 같은 규칙을 쓴다.
                if (pieces[side] && !job.appliedMatrices[side].empty() && SameMatrices(job.appliedMatrices[side], job.skinMatrices))
                {
                    ++m_Frame.jobSkipped;
                    continue;
                }
                PhaseTimer timer(L"Job skinning", m_Frame.jobSkin);
                ok = pieces[side] && pieces[side]->UpdatePose(job.skinMatrices, &error);
                if (ok)
                {
                    job.appliedMatrices[side] = job.skinMatrices;
                    m_Frame.jobVertices += uint32_t(pieces[side]->GetGeometry().vertices.size());
                }
                else
                    job.appliedMatrices[side].clear();
            }
            if (!matricesOk)
            {
                job.skinError = error;
                continue;
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

    // 유한 사각 평면 절단: 내부 분할 후 전체 절단 루프의 사각형 범위를 승인한다.
    SliceStatus CutController::SliceWithPlane(const CutJob& job, ProceduralMesh& mesh,
        ProceduralMesh& other, std::vector<std::vector<uint32_t>>& loops, std::string& error) const
    {
        const SlicePlane plane = m_Plane.ToSlicePlane();
        SliceStatus status = SliceStatus::InvalidInput;
        if (!SliceProceduralMesh(mesh, plane.point, plane.normal, true, &other,
            SliceCapOption::CreateNewSectionForCap, 1, &status, &error, &loops))
            return status;
        const CutTarget* target = FindTarget(job.key.target);
        const float margin = (target != nullptr ? target->radius : m_ModelRadius) * 0.01f;
        if (!m_Plane.ContainsLoops(mesh.GetGeometry(), loops, margin, &error))
            return SliceStatus::LoopOutsidePlane;
        return SliceStatus::Ok;
    }

    // 미절단 패치를 지금 자세로 옮긴다. Enter 때만 부른다.
    bool CutController::SkinPatch(CutJob& job, ProceduralMesh& mesh, std::string& error)
    {
        const CutTarget* target = FindTarget(job.key.target);
        if (target == nullptr || target->model == nullptr || !job.source)
        {
            error = "no source";
            return false;
        }
        {
            PhaseTimer timer(L"Job matrices", m_Frame.jobMatrices);
            if (!BuildJobMatrices(*target, job, job.skinMatrices, &error))
                return false;
        }
        PhaseTimer timer(L"Job skinning", m_Frame.jobSkin);
        if (!mesh.UpdatePose(job.skinMatrices, &error))
        {
            if (error.empty())
                error = "patch has no skin";
            return false;
        }
        m_Frame.jobVertices += uint32_t(mesh.GetGeometry().vertices.size());
        return true;
    }

    // 먼저 이전 확정 결과를 모두 지우고(Ctrl+Z와 같다), 후보 작업마다 독립으로 자른다. 미절단 패치는 작업마다 버린다.
    void CutController::ConfirmAll()
    {
        DiscardCut();
        for (auto& entry : m_Jobs)
        {
            CutJob& job = entry.second;
            if (!job.candidate)
                continue;
            ConfirmJob(job);
        }
        for (CutTarget& target : m_Targets)
        {
            UpdateSkinMask(target);     // 스켈레탈 확정마다가 아니라 한 번에 덮어쓴다
            UpdateBrokenJoints(target);
        }
    }

    // 후보 작업 하나를 지금 자세에서 추출하고 스키닝한 뒤 자른다. 실패한 작업은 결과가 없다.
    void CutController::ConfirmJob(CutJob& job)
    {
        job.cutAttempted = true;
        job.lastCutError.clear();
        job.cutInfo.clear();
        auto positive = std::make_shared<ProceduralMesh>();
        auto negative = std::make_shared<ProceduralMesh>();
        ExtractJob(job, *positive);
        if (job.patchStatus != SliceStatus::Ok)
        {
            job.lastCutStatus = job.patchStatus;
            job.lastCutError = job.patchError;
            return;
        }
        if (job.key.kind == CutJobKind::StaticMesh)
        {
            // 스태틱은 ReadStaticMesh가 이미 같은 RigidPoint로 정점을 이번 프레임 world에 옮겼으므로 SkinPatch(UpdatePose)를 건너뛴다.
            // 정점 버전이 오르지 않아도 SliceProceduralMesh가 조각에 새 버전을 주므로 괜찮다. 행렬만 채운다.
            const CutTarget* target = FindTarget(job.key.target);
            bool matricesOk = false;
            if (target == nullptr || target->model == nullptr || !job.source)
                job.lastCutError = "no source";
            else
            {
                PhaseTimer timer(L"Job matrices", m_Frame.jobMatrices);
                matricesOk = BuildJobMatrices(*target, job, job.skinMatrices, &job.lastCutError);
                if (matricesOk && (job.skinMatrices.empty() || job.skinMatrices[0].empty()))
                {
                    matricesOk = false;
                    job.lastCutError = "empty rigid matrix";
                }
            }
            if (!matricesOk)
            {
                job.lastCutStatus = SliceStatus::InvalidInput;
                return;
            }
        }
        else if (!SkinPatch(job, *positive, job.lastCutError))
        {
            job.lastCutStatus = SliceStatus::InvalidInput;
            return;
        }
        const double originalArea = TotalArea(positive->GetGeometry(), MeshFaceKind::Surface);
        std::vector<std::vector<uint32_t>> loops;
        {
            PhaseTimer timer(L"Cut job", m_Frame.cut);
            const int64_t start = SystemTime::GetCurrentTick();
            job.lastCutStatus = SliceWithPlane(job, *positive, *negative, loops, job.lastCutError);
            job.cutMs = ElapsedMs(start);
        }
        if (job.lastCutStatus != SliceStatus::Ok)
            return;
        job.confirmedNormal = m_Plane.ToSlicePlane().normal;
        job.positive = std::move(positive);
        job.negative = std::move(negative);
        job.appliedMatrices[0].clear();     // 새 조각은 첫 SkinJobs에서 반드시 갱신한다
        job.appliedMatrices[1].clear();
        if (job.key.kind == CutJobKind::StaticMesh)
        {
            // 조각은 ReadStaticMesh가 이번 프레임 world로 이미 옮겼다. 그 world를 기준으로 이후 이동을 행렬로만 표현한다.
            job.cutWorld = job.skinMatrices[0][0];
            DirectX::XMStoreFloat4x4(&job.pieceWorld, DirectX::XMMatrixIdentity());
        }
        job.haveConfirmed = true;
        job.confirmedGeneration = ++m_Generation;
        SetOriginalVisible(job, false);
        SetBreakOf(job);
        const double area = TotalArea(job.positive->GetGeometry(), MeshFaceKind::Surface)
            + TotalArea(job.negative->GetGeometry(), MeshFaceKind::Surface);
        wchar_t info[200];
        swprintf_s(info, L"loops %zu  + %zu faces  - %zu faces  area diff %.1e", loops.size(),
            job.positive->GetGeometry().faces.size(), job.negative->GetGeometry().faces.size(), area - originalArea);
        job.cutInfo = info;
    }

    void CutController::Update()
    {
        ScopedTimer _prof(L"Patch Pose Update");

        // 성능 측정 초기화
        const CutFrameStats previous = m_Frame;
        m_Frame = CutFrameStats();
        CutPhaseTime* phases[] = { &m_Frame.skeleton, &m_Frame.poseMatrices, &m_Frame.cacheSkin, &m_Frame.pick, &m_Frame.segments,
            &m_Frame.jobUpdate, &m_Frame.extract, &m_Frame.jobMatrices, &m_Frame.jobSkin, &m_Frame.cut, &m_Frame.confirm };
        const CutPhaseTime* before[] = { &previous.skeleton, &previous.poseMatrices, &previous.cacheSkin, &previous.pick, &previous.segments,
            &previous.jobUpdate, &previous.extract, &previous.jobMatrices, &previous.jobSkin, &previous.cut, &previous.confirm };
        for (size_t i = 0; i < sizeof(phases) / sizeof(phases[0]); ++i)
            phases[i]->average = before[i]->average;
        m_Frame.targets = uint32_t(m_Targets.size());
        for (const CutTarget& target : m_Targets)
            m_Frame.palettes += uint32_t(target.palettes.size());


        for (CutTarget& target : m_Targets)
        {
            UpdateTarget(target);
            InitCutStaticMesh(target);
        }

        if (m_PlaneValid)
            UpdateCandidates();
        else
        {
            PhaseTimer timer(L"Job update", m_Frame.jobUpdate);
            ApplyDiff(DiffJobs(m_Jobs, {}), {});
        }

        // Enter: 확정 조각이 같은 프레임에 스키닝되도록 SkinJobs보다 먼저 한다.
        if (m_CutRequested)
        {
            m_CutRequested = false;
            bool anyBroken = false;
            for (const CutTarget& target : m_Targets)
                anyBroken = anyBroken || !target.brokenNodes.empty();
            if (m_PlaneValid && anyBroken)
            {
                // 끊긴 자세에서 추출하지 않도록 이번 프레임은 되돌리기만 하고, 원래 자세가 계산된 다음 프레임에 자른다.
                DiscardCut();
                m_CutRequested = true;
            }
            else if (m_PlaneValid)
            {
                PhaseTimer timer(L"Confirm cut", m_Frame.confirm);
                ConfirmAll();
            }
        }
        SkinJobs();

        for (CutPhaseTime* phase : phases)
            UpdateAverage(*phase);
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
