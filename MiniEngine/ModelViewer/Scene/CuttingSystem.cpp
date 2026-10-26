#include "CuttingSystem.h"
#include "Utility.h"
#include <cstring>
#include <cmath>

namespace Viewer
{
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
        m_StateVersion = mesh->GetStateVersion();
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
            if (!mesh || mesh->GetGeometry() != candidate.Geometry || mesh->GetStateVersion() != candidate.StateVersion)
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
        if (ValidateResults && LastError == 0) m_Job->ValidateDiagnostics();
        GameObject* object = m_Scene.FindObject(m_Owner);
        MeshComponent* mesh = object ? object->FindMeshComponent(m_Mesh) : nullptr;
        if (mesh && m_Publish && mesh->GetGeometry() == m_Job->Input && mesh->GetStateVersion() == m_StateVersion)
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
                    // B를 먼저 준비한다. A의 교체가 실패하면 B를 제거하고 입력은 유지한다.
                    ComponentId pending = InvalidComponentId;
                    try
                    {
                        MeshComponent& other = object->AddMeshComponent(results[1]);
                        const ComponentId id = other.GetId();
                        pending = id;
                        other.SetPlacement(transform, mode, DeformBinding{});
                        if (mesh->SetGeometry(results[0]))
                        {
                            mesh->SetPlacement(transform, mode, DeformBinding{});
                            LastOther = id;
                            m_StatusText = "Cut complete";
                        }
                        else object->RemoveMeshComponent(id);
                    }
                    catch (const std::exception& error)
                    {
                        if (pending && !LastOther) object->RemoveMeshComponent(pending);
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
        m_Job.reset();
        if (m_BatchActive && LastOther) ++m_BatchApplied;
        FinishBatch();
        return true;
    }

    // 장면/렌더러 종료보다 먼저 제출 작업과 입력 snapshot을 정리한다.
    void CuttingSystem::Clear()
    {
        m_Pending.clear();
        m_Job.reset();
        m_BatchActive = false;
    }
}
