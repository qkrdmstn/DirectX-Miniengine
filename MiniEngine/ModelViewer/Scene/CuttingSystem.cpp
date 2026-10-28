#include "CuttingSystem.h"
#include "Utility.h"
#include "Util/CommandLineArg.h"
#include <array>
#include <cstring>
#include <cmath>

namespace Viewer
{
    namespace
    {
        // 볼록 요소 목록의 총 부피를 합산한다.
        double TotalVolume(const MiniCollision::AggregateGeom& geom)
        {
            // 요소별 부피를 더한다.
            double volume = 0.0;
            for (const MiniCollision::ConvexElem& elem : geom.ConvexElems)
                volume += elem.GetVolume();
            return volume;
        }

        // 부모의 컴포넌트 로컬 충돌을 GPU가 쓴 로컬 평면으로 잘라 out[0]=A(양의 쪽), out[1]=B(음의 쪽)를 만든다. 부모 충돌을 쓸 수 없으면 false다.
        // GpuMeshCutter::Submit의 Constants.Plane은 a·x+b·y+c·z+d이고 results[0]은 그 값이 양인 쪽이다(CutJob::ValidateDiagnostics side 0 검사). 그래서 A=양, B=음이다.
        bool SliceParentCollision(const MeshComponent& parent, const MeshCutting::CutJob& job, std::array<MiniCollision::AggregateGeom, 2>& out)
        {
            // 부모 충돌이 이 작업의 입력 revision과 대응하는 FromModel/Inherited일 때만 진행한다.
            const ComponentCollisionStatus status = parent.GetCollisionStatus();
            if ((status != ComponentCollisionStatus::FromModel && status != ComponentCollisionStatus::Inherited)
                || parent.GetCollisionRevisionId() != job.Input->GetId() || parent.GetCollision().ConvexElems.empty())
                return false;

            // GPU가 쓴 비정규화 평면의 법선과 상수항을 함께 정규화한다(PlaneDot = Dot(N,P) - W = (ax+by+cz+d)/|n|).
            const double a = float(job.Constants.Plane.GetX()), b = float(job.Constants.Plane.GetY());
            const double c = float(job.Constants.Plane.GetZ()), d = float(job.Constants.Plane.GetW());
            const double length = std::sqrt(a * a + b * b + c * c);
            if (!(length > 0.0) || !std::isfinite(length) || !std::isfinite(d))
                return false;
            const MiniCollision::Plane plane{ { a / length, b / length, c / length }, -d / length };

            // 양의 쪽(A)과 음의 쪽(B)으로 나눈다. 빈 쪽은 호출자가 body 없는 조각으로 처리한다.
            MiniCollision::SliceAggregateGeom(parent.GetCollision(), plane, out[0], out[1]);
            return true;
        }

        // -collision-log 1이면 게시마다 부모/A/B 상태·convex 수·부피와 (A+B)/부모 비율을 출력한다.
        void LogInheritance(ComponentCollisionStatus parentStatus, double parentVolume, uint32_t parentConvex, const MeshComponent& a, const MeshComponent& b)
        {
            // 로그 옵션이 꺼져 있으면 아무것도 하지 않는다.
            uint32_t enabled = 0;
            if (!CommandLineArgs::GetInteger(L"collision-log", enabled) || enabled == 0)
                return;

            // 두 조각의 부피를 합산해 부모 대비 비율과 함께 한 줄로 출력한다.
            static const char* names[] = { "None", "FromModel", "Inherited", "Unavailable" };
            const double volumeA = TotalVolume(a.GetCollision()), volumeB = TotalVolume(b.GetCollision());
            const double ratio = parentVolume > 0.0 ? (volumeA + volumeB) / parentVolume : 0.0;
            Utility::Printf("CutCollision: parent=%s convex=%u volume=%.6f | A=%s convex=%u volume=%.6f reason=%s | B=%s convex=%u volume=%.6f reason=%s | ratio=%.6f\n",
                names[(int)parentStatus], parentConvex, parentVolume,
                names[(int)a.GetCollisionStatus()], (uint32_t)a.GetCollision().ConvexElems.size(), volumeA, a.GetCollisionReason(),
                names[(int)b.GetCollisionStatus()], (uint32_t)b.GetCollision().ConvexElems.size(), volumeB, b.GetCollisionReason(), ratio);
        }
    }

    // 단일 대상 요청이 진행 중인 일괄 절단에 끼어들지 않도록 검사한다.
    bool CuttingSystem::Request(ObjectId owner, ComponentId meshId, const MeshCutting::CutPlaneData& plane, bool publish, float worldEpsilon, uint32_t capMode)
    {
        if (IsBusy()) { m_StatusText = "Busy - wait for the current cut"; return false; }
        return Submit(owner,meshId,plane,publish,worldEpsilon,capMode);
    }

    // pose와 인덱스는 GPU에 유지하며 요청의 소유·revision·월드 배치만 고정한다.
    bool CuttingSystem::Submit(ObjectId owner, ComponentId meshId, const MeshCutting::CutPlaneData& plane, bool publish, float worldEpsilon, uint32_t capMode)
    {
        if (m_Job) { m_StatusText = "Busy - wait for the current cut"; return false; }
        GameObject* object = m_Scene.FindObject(owner);
        MeshComponent* mesh = object ? object->FindMeshComponent(meshId) : nullptr;
        // 바닥 등 절단 제외 객체의 직접 요청도 공통으로 거절한다.
        if (object && !object->IsCuttable())
        { m_StatusText = "Cut target is not cuttable"; return false; }
        // simulate 메시는 body가 sleep이거나 일시정지일 때만 자른다.
        const char* movingReason = nullptr;
        if (mesh && mesh->IsSimulating() && !CutTransaction::CanCut(m_Physics, owner, meshId, movingReason))
        { m_StatusText = movingReason; return false; }

        // 기존 GPU 준비 및 section 지원 조건을 검사한다.
        if (!mesh || !mesh->HasPoseBounds() || !mesh->GetPosedGeometry().IsReady())
        { m_StatusText = "Cut target is not ready"; return false; }
        const auto geometry = mesh->GetGeometry();
        for (size_t i = 0; i < geometry->GetSections().size(); ++i)
            if (geometry->GetMesh(i).psoFlags & PSOFlags::kHasSkin)
            { m_StatusText = "Skeletal cutting is not enabled"; return false; }
        const Math::AffineTransform world = mesh->GetWorldTransform(object->GetTransform());
        try
        {
            std::vector<Math::BoundingSphere> bounds;
            for (size_t i=0;i<geometry->GetSections().size();++i) bounds.push_back(mesh->GetLocalSectionBounds(i));
            m_Job = MeshCutting::GpuMeshCutter::Submit(geometry, mesh->GetPosedGeometry(), world, plane, worldEpsilon, capMode,bounds,ValidateResults);
        }
        catch (const std::exception& error)
        {
            Utility::Printf("Static cut rejected: %s\n", error.what());
            m_StatusText = "Cut rejected - unsupported input or resource limit";
            return false;
        }
        m_EditGeneration = mesh->GetEditGeneration();
        m_PhysicsPoseGeneration = mesh->GetPhysicsPoseGeneration();
        m_Owner = owner; m_Mesh = meshId; m_World = Math::Matrix4(world); m_Publish = publish;
        m_StatusText = "Cutting";
        return true;
    }

    // collider 조회 API가 반환한 Static 후보를 고정해 순차 GPU 절단을 요청한다.
    bool CuttingSystem::RequestAll(const MeshCutting::CutPlaneData& plane, uint32_t capMode)
    {
        if (IsBusy()) { m_StatusText = "Busy - wait for the current cut"; return false; }
        const auto candidates = m_Physics.QueryCutCandidates(plane);
        m_Pending.assign(candidates.begin(),candidates.end());
        m_PendingPlane = plane; m_PendingCapMode = capMode;
        Utility::Printf("Static cut candidates: count=%zu\n",m_Pending.size());
        if (m_Pending.empty()) { m_StatusText = "No static mesh in the cut area"; return false; }
        m_BatchActive = true; m_BatchApplied = 0;
        SubmitPending();
        return IsBusy();
    }

    // 요청 시점 목록만 순회해 새로 생성한 B를 같은 입력으로 다시 자르지 않는다.
    void CuttingSystem::SubmitPending()
    {
        if (m_Job) return;
        while (!m_Pending.empty())
        {
            const CutCandidate candidate = m_Pending.front(); m_Pending.pop_front();
            GameObject* object = m_Scene.FindObject(candidate.Owner);
            MeshComponent* mesh = object ? object->FindMeshComponent(candidate.Mesh) : nullptr;
            if (!mesh || mesh->GetGeometry() != candidate.Geometry || mesh->GetEditGeneration() != candidate.EditGeneration)
            { m_StatusText = "Cancelled - cut target changed"; continue; }
            const Math::Matrix4 world(mesh->GetWorldTransform(object->GetTransform()));
            if (std::memcmp(&world,&candidate.World,sizeof(world)) != 0)
            { m_StatusText = "Cancelled - cut target moved"; continue; }
            if (Submit(candidate.Owner,candidate.Mesh,m_PendingPlane,true,1.0e-5f,m_PendingCapMode)) return;
        }
        FinishBatch();
    }

    // 성공한 후보가 있으면 전체 요청의 완료 상태를 남긴다.
    void CuttingSystem::FinishBatch()
    {
        if (!m_BatchActive || IsBusy()) return;
        if (m_BatchApplied) m_StatusText = "Cut complete";
        Utility::Printf("Static cut batch complete: published=%u\n",m_BatchApplied);
        m_BatchActive = false;
    }

    // 입력 변경·삭제·overflow는 원래 geometry를 유지하고 열린 경계는 cap 없이 게시한다.
    // simulate 대상은 body 교체 transaction(CutTransaction)이 준비·검증을 통과할 때만 게시한다.
    bool CuttingSystem::Update()
    {
        if (!m_Job || !m_Job->Poll()) return false;
        LastGpuMs = m_Job->GpuMilliseconds; LastReadyMs = m_Job->ReadyMilliseconds;
        LastError = m_Job->Status[0]; LastSegments = m_Job->Status[1];
        LastUncapped = m_Job->Status[2]; LastCaps = m_Job->Status[3]; LastOther = InvalidComponentId;
        LastOwner = m_Owner;
        m_StatusText = (LastError & 1) ? "Cut area is too small - enlarge the plane" :
            LastError ? "Cut failed - original mesh kept" :
            m_Job->HasSplit() ? (m_Publish ? "Cancelled - cut target changed" : "Cut measured") : "No split - plane missed or only touched the mesh";
        if (m_Job->HasSplit() && LastCaps < ExpectedMinimumLoops)
            throw std::runtime_error("Expected closed cap loop was not generated");

        // Scene을 바꾸기 전에 결과 한 칸을 확보해 게시 후 기록이 실패하지 않게 한다.
        ReserveResultSlot();
        GameObject* object = m_Scene.FindObject(m_Owner);
        MeshComponent* mesh = object ? object->FindMeshComponent(m_Mesh) : nullptr;

        // 분할이 있고 입력이 그대로면 부모 충돌을 같은 로컬 평면으로 잘라 둔다. 게시 중 A의 geometry가 바뀌므로 부모 값도 지금 기록한다.
        std::array<MiniCollision::AggregateGeom, 2> inherited;
        bool hasInherited = false;
        ComponentCollisionStatus parentStatus = ComponentCollisionStatus::None;
        double parentVolume = 0.0;
        uint32_t parentConvex = 0;
        if (m_Job->HasSplit() && LastError == 0 && mesh && mesh->GetGeometry() == m_Job->Input)
        {
            parentStatus = mesh->GetCollisionStatus();
            parentVolume = TotalVolume(mesh->GetCollision());
            parentConvex = (uint32_t)mesh->GetCollision().ConvexElems.size();
            hasInherited = SliceParentCollision(*mesh, *m_Job, inherited);
        }

        // 검증 모드에서는 A/B 정점이 계승 hull 안에 있는지도 함께 확인한다.
        if (ValidateResults && LastError == 0)
        {
            if (hasInherited)
            {
                m_Job->ContainmentHulls = inherited;
                m_Job->InheritanceActive = true;
            }
            m_Job->ValidateDiagnostics();
        }

        // 진단 주입이 켜져 있으면 지정한 쪽 충돌을 비운다.
        if (hasInherited && DiagEmptySide >= 0)
            inherited[DiagEmptySide].ConvexElems.clear();

        const char* failure = nullptr;
        if (mesh && m_Publish && mesh->GetGeometry() == m_Job->Input && mesh->GetEditGeneration() == m_EditGeneration)
        {
            const Math::Matrix4 current(mesh->GetWorldTransform(object->GetTransform()));
            if (std::memcmp(&current, &m_World, sizeof(current)) == 0)
            {
                std::array<std::shared_ptr<const GeometryRevision>, 2> results;
                try { results = m_Job->BuildResults(); }
                catch (const std::exception& error)
                {
                    LastError = 0x100;
                    Utility::Printf("Cut result rejected: %s\n",error.what());
                }
                if (results[0] && results[1])
                {
                    const SceneTransform transform = mesh->GetTransform();
                    const MeshTransformMode mode = mesh->GetTransformMode();
                    const bool physical = mesh->IsSimulating();
                    CutTransaction transaction;
                    // 준비 단계: 할당과 실패가 가능한 B 컴포넌트와 A 교체 자원을 모두 만든다. 실패하면 준비물만 해제되고 Scene은 그대로다.
                    try
                    {
                        const char* emptyReason = hasInherited ? "no convex after slice" : "parent has no collision";
                        std::unique_ptr<MeshComponent> preparedB = object->PrepareMeshComponent(results[1]);
                        preparedB->SetPlacement(transform, mode, DeformBinding{});
                        preparedB->SetInheritedCollision(std::move(inherited[1]), results[1]->GetId(), emptyReason);
                        const ComponentId id = preparedB->GetId();
                        auto preparedA = mesh->PrepareGeometryReplacement(results[0], transform, mode, DeformBinding{}, std::move(inherited[0]), emptyReason);

                        // simulate 대상이면 body 준비와 검증까지 끝내고, 실패하면 아무것도 바꾸지 않는다.
                        if (preparedA && physical && !transaction.Prepare(m_Physics, m_Budget, *object, *mesh,
                            m_PhysicsPoseGeneration, preparedA->Collision, preparedB->GetCollision(), id))
                        {
                            LastError = 0x200;
                            failure = transaction.GetFailure();
                            Utility::Printf("Cut physics transaction failed: owner=%llu mesh=%llu reason=%s\n",
                                static_cast<unsigned long long>(m_Owner), static_cast<unsigned long long>(m_Mesh), failure);
                        }

                        // commit 단계: 할당 없이 A를 교체하고 B를 공개한 뒤 body를 교체한다.
                        else if (preparedA)
                        {
                            mesh->CommitGeometryReplacement(*preparedA);
                            MeshComponent& other = object->CommitMeshComponent(std::move(preparedB));
                            if (physical)
                                transaction.Commit(*mesh, other);
                            LogInheritance(parentStatus, parentVolume, parentConvex, *mesh, other);
                            LastOther = id;
                            m_StatusText = "Cut complete";
                        }
                    }
                    catch (const std::exception& error)
                    {
                        LastError = 0x100;
                        Utility::Printf("Cut publish rejected: %s\n",error.what());
                    }
                }
            }
        }
        LastAppliedMs = LastOther ? std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-m_Job->Started).count() : 0;
        if (LastError) m_StatusText = (LastError & 1) ? "Cut area is too small - enlarge the plane" : "Cut failed - original mesh kept";
        Utility::Printf("Static cut result: %s owner=%llu mesh=%llu\n",m_StatusText,
            static_cast<unsigned long long>(m_Owner),static_cast<unsigned long long>(m_Mesh));
        Utility::Printf("Static cut applied: elapsedMs=%.3f published=%u\n",LastAppliedMs,LastOther?1u:0u);
        Utility::Printf("Static cut classification: surfaceSides=%u split=%u\n",m_Job->Status[5]&3,m_Job->HasSplit()?1u:0u);
        Utility::Printf("Static cut: gpuMs=%.6f readyMs=%.3f segments=%u loops=%u uncappedSegments=%u error=%u other=%llu\n",
            LastGpuMs, LastReadyMs, LastSegments, LastCaps, LastUncapped, LastError,
            static_cast<unsigned long long>(LastOther));
        Utility::Printf("Static cut cap diagnostics: failedEarSegments=%u badBoundaryMask=%u\n",m_Job->Status[6],m_Job->Status[7]);
        Utility::Printf("Static cut stages ms: clip=%.6f boundary=%.6f cap=%.6f finalize=%.6f\n",
            m_Job->StageMs[0],m_Job->StageMs[1],m_Job->StageMs[2],m_Job->StageMs[3]);

        // 이번 작업의 결과를 소비용 목록에 남긴다.
        CutResultEntry entry;
        entry.Owner = m_Owner;
        entry.Mesh = m_Mesh;
        entry.Other = LastOther;
        entry.RevisionId = (LastOther && mesh) ? mesh->GetGeometry()->GetId() : m_Job->Input->GetId();
        entry.Status = LastOther ? CutResultStatus::Published : LastError ? CutResultStatus::Failed :
            !m_Job->HasSplit() ? CutResultStatus::NoSplit : !m_Publish ? CutResultStatus::Measured : CutResultStatus::Cancelled;
        entry.Error = LastError;
        entry.Reason = failure;
        entry.GpuMs = LastGpuMs;
        entry.ReadyMs = LastReadyMs;
        m_Results.push_back(entry);

        m_Job.reset();
        if (m_BatchActive && LastOther) ++m_BatchApplied;
        FinishBatch();
        return true;
    }

    // 가득 찬 목록에서는 가장 오래된 결과를 버리고, 이후 push_back이 할당하지 않도록 최대 용량을 한 번에 확보한다.
    void CuttingSystem::ReserveResultSlot()
    {
        // 최대 용량을 확보한다. 이미 확보했다면 아무 일도 하지 않는다.
        m_Results.reserve(MaxPendingResults);

        // 가득 찼으면 가장 오래된 결과를 버리고 연속 손실은 한 번만 로그로 남긴다.
        if (m_Results.size() >= MaxPendingResults)
        {
            m_Results.erase(m_Results.begin());
            ++m_DroppedResults;
            if (!m_DropLogged)
            {
                Utility::Printf("Static cut results dropped: unconsumed results exceed %zu\n", MaxPendingResults);
                m_DropLogged = true;
            }
        }
    }

    // 쌓인 결과를 모두 넘기고 비우며 손실 로그 상태를 다시 열어 둔다.
    std::vector<CutResultEntry> CuttingSystem::DrainResults()
    {
        // 소비 후에는 새 손실 구간을 다시 한 번 로그로 남길 수 있다.
        m_DropLogged = false;
        return std::exchange(m_Results, {});
    }

    // 장면/렌더러 종료보다 먼저 제출 작업과 입력 snapshot을 정리한다.
    void CuttingSystem::Clear()
    {
        m_Pending.clear();
        m_Results.clear();
        m_Job.reset();
        m_BatchActive = false;
    }
}
