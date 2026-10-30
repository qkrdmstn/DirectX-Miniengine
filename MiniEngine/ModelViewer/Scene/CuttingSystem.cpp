#include "CuttingSystem.h"
#include "Utility.h"
#include "../../Core/GraphicsCore.h"
#include "../../Core/CommandListManager.h"
#include "Util/CommandLineArg.h"
#include <array>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <string>
#include <cstdio>

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

        // GPU DOP을 double로 옮겨 슬라이스 충돌을 재맞추고 옵션에 따라 면별 통계를 출력한다.
        void RefitSideCollision(const MeshCutting::CutJob& job, uint32_t side,
            const MiniCollision::AggregateGeom& sliced, MiniCollision::AggregateGeom& out)
        {
            // DOP이 없거나 충돌이 빈 쪽은 슬라이스 결과를 그대로 유지한다.
            out = sliced;
            MiniCollision::RefitStats stats;
            stats.VolumeBefore = TotalVolume(sliced);
            stats.VolumeAfter = stats.VolumeBefore;
            stats.Kept = static_cast<int>(sliced.ConvexElems.size());
            if (job.SideDopValid[side] && job.SideDop[side].VertexCount != 0 && !sliced.ConvexElems.empty())
            {
                // 공유하는 9개 축의 투영 범위를 옮기고 로컬 단위 패딩을 적용한다.
                MiniCollision::Dop18Bounds dop{};
                for (uint32_t axis = 0; axis < 9; ++axis)
                {
                    dop.Min[axis] = double(job.SideDop[side].Min[axis]);
                    dop.Max[axis] = double(job.SideDop[side].Max[axis]);
                }
                stats = MiniCollision::RefitAggregateToDop(sliced, dop, MiniCollision::RefitDopPad, out);
            }

            // 기존 충돌 로그와 같은 옵션으로 부피 비율과 결과 개수를 남긴다.
            uint32_t enabled = 0;
            if (!CommandLineArgs::GetInteger(L"collision-log", enabled) || enabled == 0)
                return;
            const double ratio = stats.VolumeBefore > 0.0 ? stats.VolumeAfter / stats.VolumeBefore : 0.0;
            Utility::Printf("CutCollision refit: side=%u sliced=%.9g refit=%.9g ratio=%.9g Refit=%d Disjoint=%d Kept=%d\n",
                side, stats.VolumeBefore, stats.VolumeAfter, ratio, stats.Refit, stats.Disjoint, stats.Kept);
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

        // -cut-memory-mb를 signed 정수로 읽고 작업별 GPU 임시 메모리 한도로 변환한다.
        uint64_t ReadCutMemoryLimitBytes()
        {
            constexpr uint64_t DefaultMiB = 1024;
            std::wstring limitText;
            if (!CommandLineArgs::GetString(L"cut-memory-mb", limitText))
                return DefaultMiB * 1024 * 1024;

            // 완전한 정수 문자열만 허용하고 범위 밖의 유효값은 1..1024로 제한한다.
            try
            {
                size_t consumed = 0;
                const long long requested = std::stoll(limitText, &consumed, 10);
                if (consumed != limitText.size())
                    throw std::invalid_argument("trailing characters");
                const uint64_t megabytes = static_cast<uint64_t>(std::max(1LL, std::min(requested, 1024LL)));
                return megabytes * 1024 * 1024;
            }
            catch (const std::exception&)
            {
                Utility::Printf("Invalid -cut-memory-mb value; using default 1024\n");
                return DefaultMiB * 1024 * 1024;
            }
        }
    }

    // 예외 문자열을 복사하고 거절 순간의 입력·세대·메모리 수치를 두 줄로 기록한다.
    void CuttingSystem::RejectInput(ObjectId owner, ComponentId mesh, const GeometryRevision& geometry,
        uint64_t requestedWorking, const char* reason) noexcept
    {
        // 예외 수명과 무관한 UI 문자열을 고정 크기 멤버 버퍼에 보존한다.
        std::snprintf(m_RejectionText, sizeof(m_RejectionText), "Cut rejected: %s", reason ? reason : "Unknown input error");
        m_StatusText = m_RejectionText;

        // 유효한 section 메타데이터만 읽으며 진단 실패도 원래 거절을 가리지 않는다.
        uint64_t triangles = 0;
        try
        {
            for (size_t i = 0; i < geometry.GetSections().size(); ++i)
            {
                const Mesh& section = geometry.GetMesh(i);
                for (uint32_t d = 0; d < section.numDraws; ++d)
                    triangles += section.draw[d].primCount / 3;
            }
        }
        catch (...) {}
        const auto& parent = geometry.GetGenerated();
        const uint32_t parentGeneration = parent ? parent->Generation : 0;
        const uint64_t lineageBytes = parent ? parent->LineageBytes : 0;

        // 각 로그가 Utility::Printf의 256-byte 버퍼에 들어가도록 원인 길이를 제한한다.
        Utility::Printf("Cut rejected: %.64s owner=%llu mesh=%llu revision=%llu T=%llu 9T=%llu\n",
            reason ? reason : "Unknown input error", static_cast<unsigned long long>(owner),
            static_cast<unsigned long long>(mesh), static_cast<unsigned long long>(geometry.GetId()),
            static_cast<unsigned long long>(triangles), static_cast<unsigned long long>(triangles * 9));
        Utility::Printf("Cut budget: parentGen=%u nextGen=%llu lineage=%llu retained=%llu working=%llu limit=%llu reserved=%llu\n",
            parentGeneration, static_cast<unsigned long long>(uint64_t(parentGeneration) + 1),
            static_cast<unsigned long long>(lineageBytes), static_cast<unsigned long long>(lineageBytes + triangles * 9 * 240),
            static_cast<unsigned long long>(requestedWorking), static_cast<unsigned long long>(m_MemoryLimitBytes),
            static_cast<unsigned long long>(m_ReservedBytes));
    }

    // 단일 대상 요청이 진행 중인 일괄 절단에 끼어들지 않도록 검사한다.
    bool CuttingSystem::Request(ObjectId owner, ComponentId meshId, const MeshCutting::CutPlaneData& plane, bool publish, float worldEpsilon, uint32_t capMode)
    {
        if (IsBusy()) { m_StatusText = "Busy - wait for the current cut"; return false; }
        m_MemoryLimitBytes = ReadCutMemoryLimitBytes();
        m_PeakReservedBytes = 0;
        m_PeakRetiredBytes = 0;
        m_UpdateMaxMs = 0;
        m_ProcessedMaxPerFrame = 0;
        return Submit(owner,meshId,plane,publish,worldEpsilon,capMode);
    }

    // pose와 인덱스는 GPU에 유지하며 요청의 소유·revision·월드 배치만 고정한다.
    bool CuttingSystem::Submit(ObjectId owner, ComponentId meshId, const MeshCutting::CutPlaneData& plane, bool publish, float worldEpsilon, uint32_t capMode)
    {
        // 합산 예산 대기만 후보를 다음 frame에 남기도록 구별한다.
        m_SubmissionDeferred = false;

        // 이미 같은 대상의 작업이 있으면 중복 제출하지 않는다.
        if (std::any_of(m_Active.begin(), m_Active.end(), [owner, meshId](const ActiveCut& active)
            { return active.Owner == owner && active.Mesh == meshId; }))
        {
            m_StatusText = "Busy - cut target already active";
            return false;
        }

        // 요청한 소유자와 메시를 찾는다.
        GameObject* object = m_Scene.FindObject(owner);
        MeshComponent* mesh = object ? object->FindMeshComponent(meshId) : nullptr;

        // 바닥 등 절단 제외 객체의 직접 요청도 공통으로 거절한다.
        if (object && !object->IsCuttable())
        {
            m_StatusText = "Cut target is not cuttable";
            return false;
        }

        // simulate 메시는 교체할 body가 있으면 움직이는 동안에도 자른다.
        const char* movingReason = nullptr;
        if (mesh && mesh->IsSimulating() && !CutTransaction::CanCut(m_Physics, owner, meshId, movingReason))
        {
            m_StatusText = movingReason;
            return false;
        }

        // 기존 GPU 준비 및 section 지원 조건을 검사한다.
        if (!mesh || !mesh->HasPoseBounds() || !mesh->GetPosedGeometry().IsReady())
        {
            m_StatusText = "Cut target is not ready";
            return false;
        }
        const auto geometry = mesh->GetGeometry();
        for (size_t i = 0; i < geometry->GetSections().size(); ++i)
            if (geometry->GetMesh(i).psoFlags & PSOFlags::kHasSkin)
            {
                m_StatusText = "Skeletal cutting is not enabled";
                return false;
            }

        // 자원 할당 전에 단독·합산 GPU 임시 예산을 예약한다.
        uint64_t estimate = 0;
        MeshCutting::CutCapacityPlan capacity;
        uint32_t forceCount = 0;
        CommandLineArgs::GetInteger(L"cut-count-first", forceCount);
        const uint64_t retainedLimit = std::max(512ull * 1024 * 1024, m_MemoryLimitBytes);
        const Math::AffineTransform world = mesh->GetWorldTransform(object->GetTransform());
        try
        {
            capacity = MeshCutting::GpuMeshCutter::PlanCapacity(*geometry, mesh->GetPosedGeometry(), world, plane, worldEpsilon, forceCount != 0);
            estimate = MeshCutting::GpuMeshCutter::EstimateWorkingBytes(*geometry, mesh->GetPosedGeometry(), ValidateResults, &capacity);
            if (!capacity.SurfaceVertices.empty())
            {
                uint64_t vertices = uint64_t(capacity.MaxSegments) * 3;
                for (uint32_t count : capacity.SurfaceVertices) vertices += count;
                const auto parent = geometry->GetGenerated();
                Utility::Printf("Static cut capacity: draws=%zu segments=%u vertices=%llu workingBytes=%llu retainedBytes=%llu retainedLimitBytes=%llu preflightAllowanceBytes=8388608\n",
                    capacity.SurfaceVertices.size(), capacity.MaxSegments,
                    static_cast<unsigned long long>(vertices), static_cast<unsigned long long>(estimate),
                    static_cast<unsigned long long>((parent ? parent->LineageBytes : 0) + vertices * 240),
                    static_cast<unsigned long long>(retainedLimit));
            }
            for (size_t draw = 0; draw < capacity.SurfaceVertices.size(); ++draw)
                Utility::Printf("Static cut capacity draw: draw=%zu vertices=%u\n", draw, capacity.SurfaceVertices[draw]);
        }
        catch (const std::exception& error)
        {
            RejectInput(owner, meshId, *geometry, estimate, error.what());
            return false;
        }
        if (estimate > m_MemoryLimitBytes)
        {
            Utility::Printf("Static cut rejected: estimatedBytes=%llu memoryLimitBytes=%llu\n",
                static_cast<unsigned long long>(estimate), static_cast<unsigned long long>(m_MemoryLimitBytes));
            m_StatusText = "Cut rejected - memory budget exceeded";
            return false;
        }
        if (estimate > m_MemoryLimitBytes - m_ReservedBytes)
        {
            m_StatusText = "Cut waiting - memory budget is busy";
            m_SubmissionDeferred = true;
            return false;
        }

        // GPU 제출 전에 deque 슬롯과 snapshot을 확보해 예외 시 빈 슬롯을 제거한다.
        m_Active.emplace_back();
        ActiveCut& active = m_Active.back();
        active.Owner = owner;
        active.Mesh = meshId;
        active.Input = geometry;
        active.EditGeneration = mesh->GetEditGeneration();
        active.World = Math::Matrix4(world);
        active.Publish = publish;
        active.ReservedBytes = estimate;
        m_ReservedBytes += estimate;

        try
        {
            std::vector<Math::BoundingSphere> bounds;
            for (size_t i = 0; i < geometry->GetSections().size(); ++i)
                bounds.push_back(mesh->GetLocalSectionBounds(i));
            active.Job = MeshCutting::GpuMeshCutter::Submit(geometry, mesh->GetPosedGeometry(), world, plane, worldEpsilon, capMode,bounds,ValidateResults,&capacity,retainedLimit);
        }
        catch (const std::exception& error)
        {
            m_ReservedBytes -= active.ReservedBytes;
            m_Active.pop_back();
            RejectInput(owner, meshId, *geometry, estimate, error.what());
            return false;
        }
        catch (...)
        {
            m_ReservedBytes -= active.ReservedBytes;
            m_Active.pop_back();
            Utility::Printf("Static cut rejected: unexpected submission failure\n");
            m_StatusText = "Cut rejected - unsupported input or resource limit";
            return false;
        }

        // 각 작업의 snapshot을 독립 보관하고 batch peak을 갱신한다.
        m_PeakInFlight = std::max(m_PeakInFlight, static_cast<uint32_t>(m_Active.size()));
        m_PeakReservedBytes = std::max(m_PeakReservedBytes, m_ReservedBytes);
        m_StatusText = "Cutting";
        return true;
    }

    // collider 조회 API가 반환한 Static 후보 목록을 고정해 절단 batch를 시작한다.
    bool CuttingSystem::RequestAll(const MeshCutting::CutPlaneData& plane, uint32_t capMode)
    {
        if (IsBusy()) { m_StatusText = "Busy - wait for the current cut"; return false; }
        m_MemoryLimitBytes = ReadCutMemoryLimitBytes();
        const auto candidates = m_Physics.QueryCutCandidates(plane);
        // 조회 단계에서 중복 제거된 후보 목록을 요청 순서대로 고정한다.
        m_Pending.assign(candidates.begin(), candidates.end());
        m_PendingPlane = plane; m_PendingCapMode = capMode;
        Utility::Printf("Static cut candidates: count=%zu\n",m_Pending.size());
        if (m_Pending.empty()) { m_StatusText = "No static mesh in the cut area"; return false; }
        m_BatchActive = true; m_BatchApplied = 0;
        m_BatchGpuMs = 0;
        m_BatchStarted = std::chrono::steady_clock::now();
        m_PeakInFlight = 0;
        m_PeakReservedBytes = 0;
        m_PeakRetiredBytes = 0;
        m_UpdateMaxMs = 0;
        m_ProcessedMaxPerFrame = 0;

        // 문자열 옵션을 signed 정수로 엄격히 읽고 잘못된 값은 기본치로 처리한다.
        std::wstring limitText;
        m_InFlightLimit = 2;
        if (CommandLineArgs::GetString(L"cut-inflight", limitText))
        {
            try
            {
                size_t consumed = 0;
                const long long requestedLimit = std::stoll(limitText, &consumed, 10);
                if (consumed != limitText.size())
                    throw std::invalid_argument("trailing characters");
                m_InFlightLimit = static_cast<uint32_t>(std::max(1LL, std::min(requestedLimit, 4LL)));
            }
            catch (const std::exception&)
            {
                Utility::Printf("Invalid -cut-inflight value; using default 2\n");
            }
        }
        SubmitPending();
        return IsBusy();
    }

    // 요청 시점 목록만 순회해 새로 생성한 B를 같은 입력으로 다시 자르지 않는다.
    void CuttingSystem::SubmitPending()
    {
        // 설정된 동시 작업 수까지 요청 당시 후보를 소비한다.
        while (m_Active.size() < m_InFlightLimit && !m_Pending.empty())
        {
            const CutCandidate candidate = m_Pending.front();
            GameObject* object = m_Scene.FindObject(candidate.Owner);
            MeshComponent* mesh = object ? object->FindMeshComponent(candidate.Mesh) : nullptr;
            if (!mesh || mesh->GetGeometry() != candidate.Geometry || mesh->GetEditGeneration() != candidate.EditGeneration)
            {
                m_Pending.pop_front();
                m_StatusText = "Cancelled - cut target changed";
                continue;
            }
            const Math::Matrix4 world(mesh->GetWorldTransform(object->GetTransform()));
            // simulate 대상의 물리 이동은 허용하고 일반 편집 대상의 배치는 엄격히 비교한다.
            if (!mesh->IsSimulating() && std::memcmp(&world,&candidate.World,sizeof(world)) != 0)
            {
                m_Pending.pop_front();
                m_StatusText = "Cancelled - cut target moved";
                continue;
            }

            // 제출 경로 한 곳에서 count·용량 검사하며 합산 예산 대기만 후보를 유지한다.
            Submit(candidate.Owner,candidate.Mesh,m_PendingPlane,true,1.0e-5f,m_PendingCapMode);
            if (m_SubmissionDeferred) break;
            m_Pending.pop_front();
        }
        FinishBatch();
    }

    // 삭제할 조각 또는 owner의 대기 후보를 완료 결과 없이 취소한다.
    void CuttingSystem::CancelTarget(ObjectId owner, ComponentId mesh)
    {
        // 진행 중 GPU 작업은 기존 Update의 대상 재검색에 맡긴다.
        bool removed = false;
        for (auto it = m_Pending.begin(); it != m_Pending.end();)
            if (it->Owner == owner && (mesh == InvalidComponentId || it->Mesh == mesh))
            {
                it = m_Pending.erase(it);
                removed = true;
            }
            else
                ++it;

        // 일괄 상태를 닫은 뒤 사용자 삭제 안내를 마지막에 남긴다.
        FinishBatch();
        if (removed) m_StatusText = "Cancelled - cut target deleted";
    }

    // 성공한 후보가 있으면 전체 요청의 완료 상태와 절단 시간을 남긴다.
    // 화면 문구에는 절단 수, 절단당 평균 GPU 시간, GPU 합계와 요청 전체 경과 시간을 표시한다.
    void CuttingSystem::FinishBatch()
    {
        if (!m_BatchActive || IsBusy()) return;

        // 요청 시작부터 마지막 게시까지의 경과 시간을 구한다.
        const double elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_BatchStarted).count();

        // 게시한 절단이 있으면 시간 요약을 상태 문구로 바꾼다.
        if (m_BatchApplied)
        {
            sprintf_s(m_BatchText, "Cut complete - cuts=%u avgGpu=%.3f ms totalGpu=%.3f ms total=%.1f ms",
                m_BatchApplied, m_BatchGpuMs / m_BatchApplied, m_BatchGpuMs, elapsedMs);
            m_StatusText = m_BatchText;
        }

        // 기존 로그와 예산 진단을 출력한다.
        Utility::Printf("Static cut batch complete: published=%u elapsedMs=%.3f peakActiveJobs=%u activeLimit=%u\n",
            m_BatchApplied, elapsedMs, m_PeakInFlight, m_InFlightLimit);
        Utility::Printf("Static cut budget: limitBytes=%llu peakReservedBytes=%llu peakRetiredLogicalBytes=%llu updateMaxMs=%.3f processedMaxPerFrame=%u\n",
            static_cast<unsigned long long>(m_MemoryLimitBytes), static_cast<unsigned long long>(m_PeakReservedBytes),
            static_cast<unsigned long long>(m_PeakRetiredBytes), m_UpdateMaxMs, m_ProcessedMaxPerFrame);
        m_BatchActive = false;
    }

    // GPU 완료 결과를 제한된 개수만 처리해 같은 frame의 Scene 게시를 묶는다.
    bool CuttingSystem::Update()
    {
        // scene thread에서 retired 자원 수를 관찰하고 완료 항목을 비대기 수집한다.
        const auto started = std::chrono::steady_clock::now();
        m_PeakRetiredBytes = std::max(m_PeakRetiredBytes, Graphics::g_CommandManager.GetRetiredResourceBytes());
        Graphics::g_CommandManager.CollectRetiredResources();

        // 입력 순서를 유지하며 ready front만 최대 active limit까지 반영한다.
        uint32_t processed = 0;
        while (processed < m_InFlightLimit && !m_Active.empty())
        {
            if (!UpdateOne())
                break;
            ++processed;
        }

        // batch 진단에 frame 최대 처리 시간과 완료 개수를 기록한다.
        m_UpdateMaxMs = std::max(m_UpdateMaxMs,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        m_ProcessedMaxPerFrame = std::max(m_ProcessedMaxPerFrame, processed);
        FinishBatch();
        return processed != 0;
    }

    // 입력 변경·삭제·overflow는 원래 geometry를 유지하고 열린 경계는 cap 없이 게시한다.
    // GPU DOP으로 충돌을 재맞춘 뒤 simulate 대상은 body 교체 transaction 검증을 통과할 때만 게시한다.
    bool CuttingSystem::UpdateOne()
    {
        // front의 GPU 완료를 기다리지 않고 확인한다.
        if (m_Active.empty() || !m_Active.front().Job->Poll()) return false;
        ActiveCut& active = m_Active.front();
        MeshCutting::CutJob& job = *active.Job;

        // 가능한 2-body 물리 게시가 현재 frame 예산을 넘으면 결과와 active 순서를 유지한다.
        if (active.Publish && job.Status[0] == 0 && job.HasSplit())
        {
            GameObject* target = m_Scene.FindObject(active.Owner);
            MeshComponent* targetMesh = target ? target->FindMeshComponent(active.Mesh) : nullptr;
            const bool current = targetMesh && targetMesh->GetGeometry() == active.Input &&
                targetMesh->GetEditGeneration() == active.EditGeneration;
            if (current && targetMesh->IsSimulating() && m_Budget.ShouldDefer(2))
                return false;
        }

        LastGpuMs = job.GpuMilliseconds;
        LastReadyMs = job.ReadyMilliseconds;
        LastError = job.Status[0];
        LastSegments = job.Status[1];
        LastUncapped = job.Status[2];
        LastCaps = job.Status[3];
        LastOther = InvalidComponentId;
        LastOwner = active.Owner;
        m_StatusText = (LastError & 1) ? "Cut area is too small - enlarge the plane" :
            LastError ? "Cut failed - original mesh kept" :
            job.HasSplit() ? (active.Publish ? "Cancelled - cut target changed" : "Cut measured") : "No split - plane missed or only touched the mesh";
        if (job.HasSplit() && LastCaps < ExpectedMinimumLoops)
            throw std::runtime_error("Expected closed cap loop was not generated");

        // Scene을 바꾸기 전에 결과 한 칸을 확보해 게시 후 기록이 실패하지 않게 한다.
        ReserveResultSlot();
        GameObject* object = m_Scene.FindObject(active.Owner);
        MeshComponent* mesh = object ? object->FindMeshComponent(active.Mesh) : nullptr;

        // 분할이 있고 입력이 그대로면 부모 충돌과 게시 전 부모 값을 기록한다.
        std::array<MiniCollision::AggregateGeom, 2> sliced;
        std::array<MiniCollision::AggregateGeom, 2> inherited;
        bool hasInherited = false;
        ComponentCollisionStatus parentStatus = ComponentCollisionStatus::None;
        double parentVolume = 0.0;
        uint32_t parentConvex = 0;
        if (job.HasSplit() && LastError == 0 && mesh && mesh->GetGeometry() == active.Input)
        {
            parentStatus = mesh->GetCollisionStatus();
            parentVolume = TotalVolume(mesh->GetCollision());
            parentConvex = (uint32_t)mesh->GetCollision().ConvexElems.size();
            hasInherited = SliceParentCollision(*mesh, job, sliced);

            // 진단과 게시 준비 전에 재맞추되 슬라이스 원본은 포함 검증용으로 보존한다.
            if (hasInherited)
            {
                for (uint32_t side = 0; side < 2; ++side)
                    RefitSideCollision(job, side, sliced[side], inherited[side]);
            }
        }

        // 검증 모드에서는 A/B 정점이 계승 hull 안에 있는지도 함께 확인한다.
        if (ValidateResults && LastError == 0)
        {
            if (hasInherited)
            {
                job.ContainmentHulls = sliced;
                job.RefitContainmentHulls = inherited;
                job.InheritanceActive = true;
            }
            job.ValidateDiagnostics();
        }

        // 진단 주입이 켜져 있으면 지정한 쪽 충돌을 비운다.
        if (hasInherited && DiagEmptySide >= 0)
            inherited[DiagEmptySide].ConvexElems.clear();

        // 입력 자격을 다시 확인하고 현재 배치에 절단 결과를 게시한다.
        const char* failure = nullptr;
        if (mesh && active.Publish && mesh->GetGeometry() == active.Input && mesh->GetEditGeneration() == active.EditGeneration)
        {
            const Math::Matrix4 current(mesh->GetWorldTransform(object->GetTransform()));
            // 물리 이동은 로컬 결과를 무효화하지 않으며 현재 배치로 게시한다.
            if (mesh->IsSimulating() || std::memcmp(&current, &active.World, sizeof(current)) == 0)
            {
                std::array<std::shared_ptr<const GeometryRevision>, 2> results;
                try
                {
                    results = job.BuildResults();
                }
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
                    // 준비 단계에서 B 컴포넌트와 A 교체 자원을 모두 만든다.
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
                            preparedA->Collision, preparedB->GetCollision(), id))
                        {
                            LastError = 0x200;
                            failure = transaction.GetFailure();
                            Utility::Printf("Cut physics transaction failed: owner=%llu mesh=%llu reason=%s\n",
                                static_cast<unsigned long long>(active.Owner), static_cast<unsigned long long>(active.Mesh), failure);
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

        // 게시 여부와 GPU 진단 수치를 기록한다.
        LastAppliedMs = LastOther ? std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-job.Started).count() : 0;
        if (LastError)
            m_StatusText = (LastError & 1) ? "Cut area is too small - enlarge the plane" : "Cut failed - original mesh kept";
        Utility::Printf("Static cut result: %s owner=%llu mesh=%llu\n",m_StatusText,
            static_cast<unsigned long long>(active.Owner),static_cast<unsigned long long>(active.Mesh));
        Utility::Printf("Static cut applied: elapsedMs=%.3f published=%u\n",LastAppliedMs,LastOther?1u:0u);
        Utility::Printf("Static cut classification: surfaceSides=%u split=%u\n",job.Status[5]&3,job.HasSplit()?1u:0u);
        Utility::Printf("Static cut: gpuMs=%.6f readyMs=%.3f segments=%u loops=%u uncappedSegments=%u error=%u other=%llu\n",
            LastGpuMs, LastReadyMs, LastSegments, LastCaps, LastUncapped, LastError,
            static_cast<unsigned long long>(LastOther));

        // KDop 뒤에서 수집한 cap 라운드·표시·제거 합계도 출력한다.
        Utility::Printf("Static cut cap diagnostics: failedEarSegments=%u badBoundaryMask=%u capRounds=%u capMarked=%u capRemoved=%u\n",
            job.Status[6],job.Status[7],job.CapRounds,job.CapMarked,job.CapRemoved);

        Utility::Printf("Static cut stages ms: clip=%.6f boundary=%.6f cap=%.6f finalize=%.6f\n",
            job.StageMs[0],job.StageMs[1],job.StageMs[2],job.StageMs[3]);

        // 이번 작업의 결과를 소비용 목록에 남긴다.
        CutResultEntry entry;
        entry.Owner = active.Owner;
        entry.Mesh = active.Mesh;
        entry.Other = LastOther;
        entry.RevisionId = (LastOther && mesh) ? mesh->GetGeometry()->GetId() : active.Input->GetId();
        entry.Status = LastOther ? CutResultStatus::Published : LastError ? CutResultStatus::Failed :
            !job.HasSplit() ? CutResultStatus::NoSplit : !active.Publish ? CutResultStatus::Measured : CutResultStatus::Cancelled;
        entry.Error = LastError;
        entry.Reason = failure;
        entry.GpuMs = LastGpuMs;
        entry.ReadyMs = LastReadyMs;
        entry.StageMs = job.StageMs;
        m_Results.push_back(entry);

        // 완료한 작업을 해제하고 묶음 진행 상태를 갱신한다.
        m_ReservedBytes -= active.ReservedBytes;
        m_Active.pop_front();
        m_PeakRetiredBytes = std::max(m_PeakRetiredBytes, Graphics::g_CommandManager.GetRetiredResourceBytes());
        if (m_BatchActive && LastOther)
        {
            ++m_BatchApplied;
            m_BatchGpuMs += LastGpuMs;
        }
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
        // 대기 후보와 게시 결과가 가진 공유 참조를 먼저 놓는다.
        m_Pending.clear();
        m_Results.clear();

        // 각 CutJob 소멸자가 자기 fence를 기다려 미게시 출력도 안전하게 정리한다.
        m_Active.clear();
        m_ReservedBytes = 0;
        m_BatchActive = false;

        // job 완료 후 사용이 끝난 retired resource를 대기 없이 수집한다.
        Graphics::g_CommandManager.CollectRetiredResources();
    }
}
