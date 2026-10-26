#include "PhysicsSystem.h"
#include "Utility.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace Viewer
{
    // Scene 소유자가 살아 있는 동안 대응 참조를 해제한다.
    PhysicsSystem::~PhysicsSystem() { Clear(); }

    // 전용 broadphase world를 만들되 잘못된 용량이나 SDK 초기화 실패는 경고 후 경계 검사 fallback으로 전환한다.
    void PhysicsSystem::Initialize(bool useJolt, uint32_t capacity)
    {
        Clear();
        m_UseJolt = useJolt;
        if (m_UseJolt && (capacity == 0 || capacity > MiniPhysics::MaxColliderCapacity))
        {
            Utility::Printf("Physics query disabled: invalid capacity %u (1-%u)\n",capacity,MiniPhysics::MaxColliderCapacity);
            m_UseJolt = false;
        }
        if (m_UseJolt)
        {
            try { m_World.Initialize(capacity); }
            catch (const std::exception& error)
            {
                Utility::Printf("Physics query disabled: %s\n",error.what());
                m_World.Shutdown();
                m_UseJolt = false;
            }
        }
        Utility::Printf("Physics query: backend=%s capacity=%u\n",m_UseJolt?"Jolt 5.6.0":"bounds fallback",capacity);
    }

    // 기존 Static 절단과 동일한 입력 지원 범위를 검사한다.
    bool PhysicsSystem::IsEligible(const MeshComponent& mesh)
    {
        if (!mesh.HasPoseBounds() || !mesh.GetPosedGeometry().IsReady()) return false;
        const auto& geometry = mesh.GetGeometry();
        if (geometry->GetSections().empty()) return false;
        for (size_t i = 0; i < geometry->GetSections().size(); ++i)
            if (geometry->GetMesh(i).psoFlags & PSOFlags::kHasSkin) return false;
        return true;
    }

    // 렌더와 같은 보수적 구에서 조회 Box를 만들며 GPU 정점을 읽지 않는다.
    void PhysicsSystem::SyncColliders()
    {
        size_t created = 0, updated = 0, removed = 0;
        m_FallbackKeys.clear();
        for (auto& entry : m_Entries) entry.second.Seen = false;
        for (const auto& object : m_Scene.GetObjects())
            for (const auto& mesh : object->GetMeshComponents())
            {
                if (!IsEligible(*mesh)) continue;
                const Key key(object->GetId(),mesh->GetId());
                Entry& entry = m_Entries[key];
                entry.Seen = true;
                entry.Candidate.Owner = key.first;
                entry.Candidate.Mesh = key.second;
                const auto geometry = mesh->GetGeometry();
                if (entry.Collider && !object->IsColliderCurrent(entry.Collider))
                {
                    object->RemoveColliderComponent(entry.Collider);
                    entry.Collider = InvalidComponentId;
                }
                if (!entry.Collider) entry.Collider = object->AddColliderComponent(mesh->GetId()).Id;
                const auto sphere = mesh->GetBoundingSphere(object->GetTransform());
                const auto center = sphere.GetCenter();
                const float radius = sphere.GetRadius();
                MiniPhysics::QueryBounds bounds;
                const float c[3] = {float(center.GetX()),float(center.GetY()),float(center.GetZ())};
                for (size_t i = 0; i < 3; ++i)
                {
                    bounds.Minimum[i] = c[i]-radius;
                    bounds.Maximum[i] = c[i]+radius;
                }
                if (entry.Handle && (bounds.Minimum != entry.Bounds.Minimum || bounds.Maximum != entry.Bounds.Maximum))
                {
                    if (m_World.UpdateQueryBox(entry.Handle,bounds)) ++updated;
                    else
                    {
                        m_Handles.erase(entry.Handle);
                        m_World.RemoveCollider(entry.Handle);
                        entry.Handle = MiniPhysics::InvalidCollider;
                    }
                }
                if (!entry.Handle && m_UseJolt)
                {
                    entry.Handle = m_World.CreateQueryBox(bounds);
                    if (entry.Handle) { m_Handles.emplace(entry.Handle,key); ++created; }
                }
                object->SetColliderQueryHandle(entry.Collider,entry.Handle);
                if (!entry.Handle) m_FallbackKeys.push_back(key);
                entry.Bounds = bounds;
                entry.Sphere = sphere;
                entry.Candidate = {object->GetId(),mesh->GetId(),geometry,mesh->GetStateVersion(),
                    Math::Matrix4(mesh->GetWorldTransform(object->GetTransform()))};
            }
        for (auto it = m_Entries.begin(); it != m_Entries.end();)
        {
            if (it->second.Seen) { ++it; continue; }
            RemoveEntry(it->second);
            it = m_Entries.erase(it);
            ++removed;
        }
        if (created || updated || removed)
            Utility::Printf("Physics colliders: created=%zu updated=%zu removed=%zu registered=%zu fallback=%zu\n",
                created,updated,removed,GetColliderCount(),GetFallbackCount());
    }

    // 요청 시점에 동기화되지 않은 geometry나 변환을 후보에 섞지 않는다.
    bool PhysicsSystem::IsCurrent(const Entry& entry) const
    {
        const auto& candidate = entry.Candidate;
        const auto* object = m_Scene.FindObject(candidate.Owner);
        const auto* mesh = object ? object->FindMeshComponent(candidate.Mesh) : nullptr;
        if (!mesh || !IsEligible(*mesh) || mesh->GetGeometry() != candidate.Geometry ||
            mesh->GetStateVersion() != candidate.StateVersion || !object->IsColliderCurrent(entry.Collider)) return false;
        const Math::Matrix4 world(mesh->GetWorldTransform(object->GetTransform()));
        return std::memcmp(&world,&candidate.World,sizeof(world)) == 0;
    }

    // broadphase의 과잉 후보를 기존 평면·경계 구 조건으로 줄인다.
    bool PhysicsSystem::Intersects(const Entry& entry, const MeshCutting::CutPlaneData& plane,
        const MiniPhysics::QueryBounds& rectangle, float epsilon)
    {
        const auto center = entry.Sphere.GetCenter();
        const float radius = entry.Sphere.GetRadius()+epsilon;
        if (std::abs(float(Math::Dot(plane.WorldPlane,Math::Vector4(center,1)))) > radius) return false;
        const float c[3] = {float(center.GetX()),float(center.GetY()),float(center.GetZ())};
        for (size_t i = 0; i < 3; ++i)
            if (c[i]+radius < rectangle.Minimum[i] || c[i]-radius > rectangle.Maximum[i]) return false;
        return true;
    }

    // 평면 두께는 후보 검색에만 쓰며 GPU의 유한 영역 판정은 유지한다.
    std::vector<CutCandidate> PhysicsSystem::QueryCutCandidates(const MeshCutting::CutPlaneData& plane, float worldEpsilon) const
    {
        if (!std::isfinite(worldEpsilon) || worldEpsilon < 0) throw std::invalid_argument("Invalid query epsilon");
        MiniPhysics::QueryBounds rectangle;
        const auto minimum = Math::Min(Math::Min(plane.WorldCorners[0],plane.WorldCorners[1]),
            Math::Min(plane.WorldCorners[2],plane.WorldCorners[3]));
        const auto maximum = Math::Max(Math::Max(plane.WorldCorners[0],plane.WorldCorners[1]),
            Math::Max(plane.WorldCorners[2],plane.WorldCorners[3]));
        rectangle.Minimum = {{float(minimum.GetX()),float(minimum.GetY()),float(minimum.GetZ())}};
        rectangle.Maximum = {{float(maximum.GetX()),float(maximum.GetY()),float(maximum.GetZ())}};
        auto search = rectangle;
        for (size_t i = 0; i < 3; ++i)
        {
            search.Minimum[i] = std::nextafter(rectangle.Minimum[i]-worldEpsilon,-std::numeric_limits<float>::max());
            search.Maximum[i] = std::nextafter(rectangle.Maximum[i]+worldEpsilon,std::numeric_limits<float>::max());
        }
        std::vector<Key> keys;
        const bool useIndex = m_UseJolt && search.IsValid();
        if (useIndex)
            for (auto handle : m_World.QueryAabb(search))
            {
                const auto entry = m_Handles.find(handle);
                if (entry != m_Handles.end()) keys.push_back(entry->second);
            }
        if (useIndex) keys.insert(keys.end(),m_FallbackKeys.begin(),m_FallbackKeys.end());
        else for (const auto& entry : m_Entries) keys.push_back(entry.first);
        std::sort(keys.begin(),keys.end());
        keys.erase(std::unique(keys.begin(),keys.end()),keys.end());
        std::vector<CutCandidate> result;
        for (const auto& key : keys)
        {
            const auto& entry = m_Entries.at(key);
            if (IsCurrent(entry) && Intersects(entry,plane,rectangle,worldEpsilon)) result.push_back(entry.Candidate);
        }
        if (ValidateQueries)
        {
            size_t expected = 0;
            for (const auto& entry : m_Entries)
                if (IsCurrent(entry.second) && Intersects(entry.second,plane,rectangle,worldEpsilon))
                {
                    ++expected;
                    if (!std::binary_search(keys.begin(),keys.end(),entry.first))
                        throw std::runtime_error("Jolt cut query missed a bounds candidate");
                }
            if (expected != result.size()) throw std::runtime_error("Jolt cut query candidate mismatch");
            Utility::Printf("Physics cut query validation passed: candidates=%zu indexed=%zu fallback=%zu\n",
                result.size(),GetColliderCount(),GetFallbackCount());
        }
        return result;
    }

    // Scene이 먼저 삭제된 경우에도 SDK body를 남기지 않는다.
    void PhysicsSystem::RemoveEntry(Entry& entry)
    {
        if (auto* object = m_Scene.FindObject(entry.Candidate.Owner)) object->RemoveColliderComponent(entry.Collider);
        m_Handles.erase(entry.Handle);
        m_World.RemoveCollider(entry.Handle);
    }

    // Scene 대응과 SDK 자원을 종료 순서대로 정리한다.
    void PhysicsSystem::Clear()
    {
        for (auto& entry : m_Entries) RemoveEntry(entry.second);
        m_Entries.clear();
        m_Handles.clear();
        m_FallbackKeys.clear();
        m_World.Shutdown();
    }

    // SDK가 처리하지 못한 항목도 기존 경계 검사로 누락 없이 유지한다.
    size_t PhysicsSystem::GetFallbackCount() const
    {
        return m_FallbackKeys.size();
    }
}
