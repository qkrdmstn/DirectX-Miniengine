#include "MeshComponent.h"
#include "ConstantBuffers.h"
#include "Renderer.h"
#include "CommandListManager.h"
#include "GraphicsCore.h"
#include "Util/CommandLineArg.h"
#include "RootSignature.h"
#include "PipelineState.h"
#include "CompiledShaders/TriangleMaskCS.h"
#include "CompiledShaders/PatchSkinBakeCS.h"
#include "ReadbackBuffer.h"
#include <cstdio>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>

using namespace Math;

namespace Viewer
{
    // 불변 원본 참조와 컴포넌트 전용 GPU 저장 공간을 준비한다.
    MeshComponent::MeshComponent(ComponentId id, std::shared_ptr<const GeometryRevision> geometry)
        : m_Id(id), m_Geometry(std::move(geometry))
    {
        // ID와 원본이 유효한지 검증한다.
        if (id == InvalidComponentId || !m_Geometry)
            throw std::invalid_argument("Invalid mesh component");
        const Model& model = *m_Geometry->GetSource();
        if (model.m_NumNodes == 0)
            throw std::invalid_argument("Mesh component requires nodes");

        // 컴포넌트 전용 pose 버퍼와 렌더 상수 버퍼를 만든다.
        m_PosedGeometry = m_Geometry->IsGenerated() ? std::make_unique<PosedGeometry>(*m_Geometry->GetGenerated())
            : std::make_unique<PosedGeometry>(model);
        m_ConstantsCPU.Create(L"Component constants upload", model.m_NumNodes * sizeof(MeshConstants));
        m_ConstantsGPU.Create(L"Component constants", model.m_NumNodes, sizeof(MeshConstants));

        // 현재 revision의 충돌을 만든다.
        RebuildCollision();
    }

    // 삼각형 수와 상태 비트를 검사하고 컴포넌트 전용 변경을 예약한다.
    bool MeshComponent::SetTriangleMask(const std::vector<uint32_t>& state)
    {
        // 생성 geometry에는 원본 삼각형 번호를 적용하지 않는다.
        if (m_Geometry->IsGenerated()) return false;
        uint64_t triangles = 0;
        uint32_t first = UINT32_MAX, last = 0;
        for (size_t s = 0; s < m_Geometry->GetSections().size(); ++s)
        {
            const Mesh& mesh = m_Geometry->GetMesh(s);
            first = std::min(first, mesh.ibOffset & ~3u);
            last = std::max(last, (mesh.ibOffset + mesh.ibSize + 3u) & ~3u);
            for (uint32_t d = 0; d < mesh.numDraws; ++d) triangles += mesh.draw[d].primCount / 3;
        }
        if (!triangles || triangles != state.size()) return false;
        for (uint32_t value : state) if (value & 0x7fff0000u) return false;

        // 최초 변경에만 IB 구간과 삼각형 상태 버퍼를 할당한다.
        if (!m_TriangleMask)
        {
            m_TriangleMask = std::make_unique<TriangleMask>();
            m_TriangleMask->TriangleCount = uint32_t(triangles);
            m_TriangleMask->FirstByte = first;
            m_TriangleMask->ByteCount = last - first;
            m_TriangleMask->State.Create(L"Component triangle state", uint32_t(triangles), 4);
            m_TriangleMask->ReplacedIB.Create(L"Component replacement IB", (last - first) / 4, 4);
        }
        if (m_TriangleMask->Pending == state) return true;
        m_TriangleMask->Pending = state;
        ++m_TriangleMask->Revision;
        return true;
    }

    // 이전 프레임이 읽는 자원을 기다린 뒤 원본 인덱스 경로로 복귀한다.
    void MeshComponent::ClearTriangleMask()
    {
        // 프레임 입력 단계에서만 자원을 해제한다.
        if (!m_TriangleMask) return;
        Graphics::g_CommandManager.IdleGPU();
        m_TriangleMask.reset();
    }

    // 상태 변경만 같은 명령 목록에서 계산하고 모든 렌더 패스가 읽을 IB로 전환한다.
    void MeshComponent::UpdateTriangleMask(GraphicsContext& context)
    {
        // 변경되지 않은 마스크에는 업로드나 dispatch가 없다.
        if (!m_TriangleMask || m_TriangleMask->Revision == m_TriangleMask->AppliedRevision) return;
        auto& mask = *m_TriangleMask;
        static RootSignature root;
        static ComputePSO pso(L"Component triangle mask");
        static bool initialized = false;
        if (!initialized)
        {
            root.Reset(5);
            root[0].InitAsConstants(0, 4);
            for (uint32_t i = 0; i < 3; ++i) root[i + 1].InitAsBufferSRV(i);
            root[4].InitAsBufferUAV(0);
            root.Finalize(L"Component triangle mask");
            pso.SetRootSignature(root);
            pso.SetComputeShader(g_pTriangleMaskCS, sizeof(g_pTriangleMaskCS));
            pso.Finalize();
            initialized = true;
        }

        // 추출과 같은 section/draw 순서로 전역 삼각형 번호를 누적한다.
        std::vector<std::array<uint32_t, 16>> draws;
        uint32_t triangle = 0;
        for (size_t s = 0; s < m_Geometry->GetSections().size(); ++s)
        {
            const Mesh& mesh = m_Geometry->GetMesh(s);
            for (uint32_t d = 0; d < mesh.numDraws; ++d)
            {
                const auto& draw = mesh.draw[d];
                if (!draw.primCount) continue;
                draws.push_back({ 0, 0, mesh.ibOffset, mesh.ibFormat == DXGI_FORMAT_R32_UINT,
                    draw.startIndex, draw.baseVertex, draw.primCount / 3, 0, triangle });
                triangle += draw.primCount / 3;
            }
        }

        // 상태를 업로드하고 원본 SRV에서 독립 UAV를 다시 만든다.
        const size_t bytes = mask.Pending.size() * 4;
        DynAlloc upload = context.ReserveUploadMemory(bytes);
        std::memcpy(upload.DataPtr, mask.Pending.data(), bytes);
        context.CopyBufferRegion(mask.State, 0, upload.Buffer, upload.Offset, bytes);
        auto& source = const_cast<ByteAddressBuffer&>(m_Geometry->GetDataBuffer());
        auto& compute = context.GetComputeContext();
        compute.SetRootSignature(root);
        compute.SetPipelineState(pso);
        compute.TransitionResource(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(mask.State, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(mask.ReplacedIB, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.SetBufferSRV(1, source);
        compute.SetDynamicSRV(2, draws.size() * sizeof(draws[0]), draws.data());
        compute.SetBufferSRV(3, mask.State);
        compute.SetBufferUAV(4, mask.ReplacedIB);
        const uint32_t parameters[] = { mask.FirstByte / 4, mask.ByteCount / 4, uint32_t(draws.size()), mask.FirstByte / 4 };
        compute.SetConstantArray(0, 4, parameters);
        const uint32_t groups = (mask.ByteCount / 4 + 63) / 64;
        compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
        context.TransitionResource(mask.ReplacedIB, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        context.TransitionResource(source, D3D12_RESOURCE_STATE_GENERIC_READ);
        mask.AppliedRevision = mask.Revision;
    }

    // 선택 section이 모두 Generated hull을 가져야 SourceFrame⁻¹를 적용해 out에 모은다. 하나라도 아니면 out을 비우고 이유를 남긴다.
    // 정적 section과 스킨 section이 섞인 컴포넌트는 하나의 강체가 될 수 없으므로 전체를 Unavailable로 취급한다.
    bool GatherSectionHulls(const std::vector<MeshCollisionStatus>& statuses, const std::vector<MiniCollision::AggregateGeom>& hulls,
        const std::vector<GeometrySection>& sections, const double inverse[3][4], MiniCollision::AggregateGeom& out, std::string& reason)
    {
        // 이전 결과를 비운다.
        static const char* names[] = { "Generated", "Skinned", "NodeAnimated", "DecodeFailed", "GenerateFailed" };
        out.ConvexElems.clear();

        // section이 없으면 쓸 수 있는 hull이 없다.
        if (sections.empty())
        {
            reason = "no sections selected";
            return false;
        }

        // section마다 범위와 상태를 검사하고 hull을 복사해 역변환한다.
        for (const GeometrySection& section : sections)
        {
            if (section.SourceMesh >= statuses.size() || section.SourceMesh >= hulls.size())
            {
                out.ConvexElems.clear();
                reason = "section " + std::to_string(section.SourceMesh) + " out of range";
                return false;
            }
            if (statuses[section.SourceMesh] != MeshCollisionStatus::Generated || hulls[section.SourceMesh].ConvexElems.empty())
            {
                out.ConvexElems.clear();
                reason = "section " + std::to_string(section.SourceMesh) + " has no hull ("
                    + names[static_cast<int>(statuses[section.SourceMesh])] + ")";
                return false;
            }
            for (MiniCollision::ConvexElem elem : hulls[section.SourceMesh].ConvexElems)
            {
                elem.ApplyAffine(inverse);
                out.ConvexElems.push_back(std::move(elem));
            }
        }

        // 모든 section의 hull을 모았다.
        return true;
    }

    // 원본 revision은 선택 section의 Model hull에 SourceFrame⁻¹를 적용하고, 생성 revision은 SetInheritedCollision이 채울 때까지 Unavailable로 둔다.
    void MeshComponent::RebuildCollision()
    {
        // 생성 revision은 Update가 SourceFrame을 쓰지 않고 정점을 GPU 결과 프레임 그대로 그린다.
        // 같은 revision에 이미 대응하는 계승 충돌은 SourceFrame이 바뀌어도 같은 프레임이므로 재기준화 없이 유지한다.
        if (m_Geometry->IsGenerated() && m_CollisionRevisionId == m_Geometry->GetId())
            return;

        // 이전 revision의 hull을 지우고 현재 revision ID를 기록한다.
        m_Collision.ConvexElems.clear();
        m_CollisionStatus = ComponentCollisionStatus::Unavailable;
        m_CollisionReason.clear();
        m_CollisionRevisionId = m_Geometry->GetId();

        // 새 생성 revision은 부모 계승이 제공될 때까지 쓸 수 없다.
        if (m_Geometry->IsGenerated())
        {
            m_CollisionReason = "generated revision: inheritance not provided";
            return;
        }

        // SourceFrame TRS의 역변환 x = S⁻¹·Rᵀ·(p − T)를 double 3x4로 만든다.
        const Model& model = *m_Geometry->GetSource();
        const SceneTransform& frame = m_DeformBinding.SourceFrame;
        DirectX::XMFLOAT4 q;
        DirectX::XMFLOAT3 scale, position;
        XMStoreFloat4(&q, frame.GetRotation());
        XMStoreFloat3(&scale, frame.GetScale());
        XMStoreFloat3(&position, frame.GetTranslation());
        const double qn = std::sqrt(double(q.x) * q.x + double(q.y) * q.y + double(q.z) * q.z + double(q.w) * q.w);
        const double x = q.x / qn, y = q.y / qn, z = q.z / qn, w = q.w / qn;
        const double rot[3][3] = {
            { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
            { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
            { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } };
        const double inverseScale[3] = { 1.0 / scale.x, 1.0 / scale.y, 1.0 / scale.z };
        const double move[3] = { position.x, position.y, position.z };
        double inverse[3][4];
        for (int i = 0; i < 3; ++i)
        {
            inverse[i][3] = 0.0;
            for (int j = 0; j < 3; ++j)
            {
                inverse[i][j] = rot[j][i] * inverseScale[i];
                inverse[i][3] -= inverse[i][j] * move[j];
            }
        }

        // 선택 section의 hull을 모아 역변환한다. 실패하면 이유를 남기고 Unavailable로 둔다.
        std::string reason;
        if (GatherSectionHulls(model.m_MeshCollisionStatus, model.m_MeshCollision, m_Geometry->GetSections(), inverse, m_Collision, reason))
            m_CollisionStatus = ComponentCollisionStatus::FromModel;
        else
            m_CollisionReason = std::move(reason);
    }

    // 현재 생성 revision에 대응하는 계승 충돌만 받아 저장하고 빈 형상은 Unavailable로 둔다.
    bool MeshComponent::SetInheritedCollision(MiniCollision::AggregateGeom geom, uint64_t revisionId, const char* emptyReason)
    {
        // 생성 revision이 아니거나 revision이 어긋나면 낡은 계승으로 보고 거절한다.
        if (!m_Geometry->IsGenerated() || revisionId != m_Geometry->GetId())
            return false;

        // 현재 revision의 충돌로 저장한다.
        m_CollisionRevisionId = revisionId;
        m_Collision = std::move(geom);

        // 비어 있으면 body 없는 조각, 아니면 Inherited로 표시한다.
        if (m_Collision.ConvexElems.empty())
        {
            m_CollisionStatus = ComponentCollisionStatus::Unavailable;
            m_CollisionReason = emptyReason;
        }
        else
        {
            m_CollisionStatus = ComponentCollisionStatus::Inherited;

            // 부착 해제 출처만 body 등록 실패 진단에 사용할 수 있도록 보존한다.
            m_CollisionReason = emptyReason && std::strcmp(emptyReason, "detached kdop") == 0 ? emptyReason : "";
        }
        return true;
    }

    // revision이 현재와 같을 때만 저장된 충돌을 돌려준다.
    const MiniCollision::AggregateGeom& MeshComponent::GetCollision() const
    {
        // revision 대응이 깨진 경우를 위한 빈 형상을 준비한다.
        static const MiniCollision::AggregateGeom empty;

        // 저장된 충돌이 현재 revision의 것일 때만 돌려준다.
        return m_CollisionRevisionId == m_Geometry->GetId() ? m_Collision : empty;
    }

    // revision이 다르면 Unavailable로 취급한다.
    ComponentCollisionStatus MeshComponent::GetCollisionStatus() const
    {
        // 저장된 상태가 현재 revision의 것일 때만 돌려주고 아니면 Unavailable로 본다.
        return m_CollisionRevisionId == m_Geometry->GetId() ? m_CollisionStatus : ComponentCollisionStatus::Unavailable;
    }

    // -collision-log 1이면 owner·컴포넌트 ID·상태·convex 수·총 부피를 출력한다.
    void MeshComponent::LogCollision(uint32_t ownerId) const
    {
        // 로그 옵션이 꺼져 있으면 아무것도 하지 않는다.
        uint32_t enabled = 0;
        if (!CommandLineArgs::GetInteger(L"collision-log", enabled) || enabled == 0)
            return;

        // 총 부피를 합산해 한 줄로 출력한다.
        static const char* names[] = { "None", "FromModel", "Inherited", "Unavailable" };
        double volume = 0.0;
        for (const MiniCollision::ConvexElem& elem : GetCollision().ConvexElems)
            volume += elem.GetVolume();
        Utility::Printf("ComponentCollision: owner=%u mesh=%u status=%s convex=%u volume=%.4f reason=%s\n",
            ownerId, (uint32_t)m_Id, names[(int)GetCollisionStatus()],
            (uint32_t)GetCollision().ConvexElems.size(), volume, m_CollisionReason.c_str());
    }

    // PosedGeometry의 GPU 완료 대기 후 상수와 geometry 소유권을 해제한다.
    MeshComponent::~MeshComponent()
    {
        Graphics::g_CommandManager.IdleGPU();
        m_PosedGeometry.reset();
        m_ConstantsCPU.Destroy();
        m_ConstantsGPU.Destroy();
    }

    // 유효한 TRS와 변환 모드를 함께 적용한다.
    bool MeshComponent::SetTransform(const SceneTransform& transform, MeshTransformMode mode)
    {
        // 새 편집 배치를 검증한 뒤 부착 월드 캐시를 해제한다.
        SceneTransform normalized = transform;
        if (!normalized.Normalize())
            return false;
        ++m_EditGeneration;
        m_Transform = normalized;
        m_TransformMode = mode;
        m_PatchFallbackWorld = false;
        m_Ready = false;
        return true;
    }

    // 부착 추종의 유효한 월드 TRS만 적용하고 사용자 편집 세대는 유지한다.
    void MeshComponent::SetAttachedWorld(const SceneTransform& world)
    {
        // 기존 배치 경로와 같은 정규화 검사를 통과한 값만 사용한다.
        SceneTransform normalized = world;
        if (!normalized.Normalize())
            return;

        // 추종은 편집이 아니므로 세대 증가 없이 렌더 준비 상태만 무효화한다.
        m_Transform = normalized;
        m_TransformMode = MeshTransformMode::WorldIndependent;
        m_PatchFallbackWorld = false;
        m_Ready = false;
    }

    // 월드 TRS를 먼저 검증한 뒤 같은 화면 배치로 owner 종속만 해제한다.
    bool MeshComponent::MakeWorldIndependent(const SceneTransform& owner)
    {
        // 이미 독립 상태면 편집 세대나 준비 상태를 바꾸지 않는다.
        if (m_TransformMode == MeshTransformMode::WorldIndependent)
            return true;

        // 전단을 버리지 않고 완전한 월드 TRS로 표현 가능한 경우에만 전환한다.
        SceneTransform world;
        if (!SceneTransform::TryFromAffine(GetWorldTransform(owner), world))
            return false;
        return SetTransform(world, MeshTransformMode::WorldIndependent);
    }

    // 정의되지 않은 SIMD 칸을 제외하고 X/Y/Z 성분의 float 비트가 모두 같은지 비교한다.
    static bool SameBits3(Vector3 a, Vector3 b)
    {
        DirectX::XMFLOAT3 fa, fb;
        XMStoreFloat3(&fa, a);
        XMStoreFloat3(&fb, b);
        return std::memcmp(&fa, &fb, sizeof(fa)) == 0;
    }

    // X/Y/Z/W 네 성분의 float 비트가 모두 같은지 비교한다.
    static bool SameBits4(Quaternion a, Quaternion b)
    {
        DirectX::XMFLOAT4 fa, fb;
        XMStoreFloat4(&fa, a);
        XMStoreFloat4(&fb, b);
        return std::memcmp(&fa, &fb, sizeof(fa)) == 0;
    }

    // WorldIndependent의 위치·회전만 갱신하고 실제로 바뀐 경우에만 물리 pose 세대를 올린다.
    bool MeshComponent::SetPhysicsPose(const Vector3& position, const Quaternion& rotation)
    {
        // WorldIndependent가 아니거나 값이 유효하지 않으면 거절한다.
        if (m_TransformMode != MeshTransformMode::WorldIndependent)
            return false;
        SceneTransform next(rotation, m_Transform.GetScale(), position);
        if (!next.Normalize())
            return false;

        // 정규화된 T/R의 사용 성분(X/Y/Z, W)이 비트까지 같으면 세대와 준비 상태를 건드리지 않는다.
        if (SameBits3(next.GetTranslation(), m_Transform.GetTranslation()) && SameBits4(next.GetRotation(), m_Transform.GetRotation()))
            return true;

        // 크기는 유지한 채 pose만 저장하고 준비 상태를 무효화한다. EditGeneration은 올리지 않는다.
        ++m_PhysicsPoseGeneration;
        m_Transform = next;
        m_Ready = false;
        return true;
    }


    // 변형 기준을 바꾸면 고정 pose snapshot도 다음 Update에서 다시 잡는다.
    bool MeshComponent::SetDeformBinding(const DeformBinding& binding)
    {
        // 새 기준을 검증한다.
        DeformBinding normalized = binding;
        if (!normalized.SourceFrame.Normalize())
            return false;

        // 기준을 저장하고 pose 관련 캐시를 무효화한다.
        ++m_EditGeneration;
        m_DeformBinding = normalized;
        m_SectionBounds.clear();
        m_FrozenNodes.clear();
        m_FrozenJoints.clear();
        m_Ready = false;

        // 새 SourceFrame으로 충돌을 다시 만든다.
        RebuildCollision();
        return true;
    }

    // 두 TRS를 모두 검증한 뒤 source-to-component와 target world 배치를 함께 적용한다.
    bool MeshComponent::SetPlacement(const SceneTransform& transform, MeshTransformMode mode, const DeformBinding& binding)
    {
        SceneTransform target = transform;
        DeformBinding source = binding;
        if (!target.Normalize() || !source.SourceFrame.Normalize())
            return false;
        SetTransform(target, mode);
        SetDeformBinding(source);
        return true;
    }

    // 다른 원본의 pose와 혼합하지 않으며 이전 GPU 결과 사용 완료 후 교체한다.
    bool MeshComponent::SetGeometry(std::shared_ptr<const GeometryRevision> geometry)
    {
        // 같은 원본의 geometry만 받는다.
        if (!geometry || geometry->GetSource() != m_Geometry->GetSource())
            return false;

        // 새 pose 버퍼를 만들고 이전 버퍼를 교체한다.
        auto posed = geometry->IsGenerated() ? std::make_unique<PosedGeometry>(*geometry->GetGenerated())
            : std::make_unique<PosedGeometry>(*geometry->GetSource());
        m_PosedGeometry.reset();
        m_PosedGeometry = std::move(posed);
        m_PatchSkinActive = false;

        // revision을 교체하고 경계·준비 상태를 무효화한다.
        ++m_EditGeneration;
        ClearTriangleMask();
        m_Geometry = std::move(geometry);
        m_SectionBounds.clear();
        m_Ready = false;

        // 이전 revision의 hull을 버리고 새 revision의 충돌을 만든다.
        RebuildCollision();
        return true;
    }

    // 교체에 필요한 pose 버퍼와 충돌 상태를 컴포넌트 밖에서 미리 만든다. 같은 원본이 아니거나 TRS가 무효면 null을 돌려준다.
    std::unique_ptr<PreparedGeometryReplacement> MeshComponent::PrepareGeometryReplacement(std::shared_ptr<const GeometryRevision> geometry,
        const SceneTransform& transform, MeshTransformMode mode, const DeformBinding& binding,
        MiniCollision::AggregateGeom collision, const char* emptyReason) const
    {
        // 같은 원본의 생성 geometry만 받는다.
        if (!geometry || !geometry->IsGenerated() || geometry->GetSource() != m_Geometry->GetSource())
            return nullptr;

        // 배치와 source 기준을 정규화해 검증한다.
        auto prepared = std::make_unique<PreparedGeometryReplacement>();
        prepared->Transform = transform;
        prepared->Binding = binding;
        if (!prepared->Transform.Normalize() || !prepared->Binding.SourceFrame.Normalize())
            return nullptr;
        prepared->Mode = mode;

        // 새 pose 버퍼를 만든다.
        prepared->Posed = std::make_unique<PosedGeometry>(*geometry->GetGenerated());

        // 계승 충돌과 상태를 확정한다. 빈 형상은 body 없는 조각이다.
        const bool empty = collision.ConvexElems.empty();
        prepared->Collision = std::move(collision);
        if (empty)
        {
            prepared->CollisionStatus = ComponentCollisionStatus::Unavailable;
            prepared->CollisionReason = emptyReason;
        }
        else
        {
            // 유효 충돌도 부착 해제 출처만 선택적으로 보존한다.
            prepared->CollisionStatus = ComponentCollisionStatus::Inherited;
            if (emptyReason && std::strcmp(emptyReason, "detached kdop") == 0)
                prepared->CollisionReason = emptyReason;
        }
        prepared->Geometry = std::move(geometry);
        return prepared;
    }

    // 준비된 값을 옮기기만 해 geometry·배치·충돌을 한 번에 바꾼다. 이전 pose 버퍼는 마지막에 해제된다.
    void MeshComponent::CommitGeometryReplacement(PreparedGeometryReplacement& prepared) noexcept
    {
        // pose 버퍼를 교체하되 이전 버퍼의 해제는 함수 끝으로 미룬다.
        std::unique_ptr<PosedGeometry> previous = std::move(m_PosedGeometry);
        m_PosedGeometry = std::move(prepared.Posed);
        m_PatchSkinActive = false;

        // SetGeometry·SetTransform·SetDeformBinding과 같은 세대 증가와 캐시 무효화를 적용한다.
        ClearTriangleMask();
        m_EditGeneration += 3;
        m_Geometry = std::move(prepared.Geometry);
        m_Transform = prepared.Transform;
        m_TransformMode = prepared.Mode;
        m_PatchFallbackWorld = false;
        m_DeformBinding = prepared.Binding;
        m_SectionBounds.clear();
        m_FrozenNodes.clear();
        m_FrozenJoints.clear();
        m_Ready = false;

        // 새 revision에 대응하는 계승 충돌을 저장한다.
        m_Collision = std::move(prepared.Collision);
        m_CollisionStatus = prepared.CollisionStatus;
        m_CollisionReason = std::move(prepared.CollisionReason);
        m_CollisionRevisionId = m_Geometry->GetId();
    }

    // 스키닝 조각은 준비된 W를 읽고 일반 메시에는 owner 종속 모드를 적용한다.
    AffineTransform MeshComponent::GetWorldTransform(const SceneTransform& owner) const
    {
        // 스키닝 조각의 조회는 렌더와 같은 W를 사용하며 편집 세대는 바꾸지 않는다.
        if (HasSkinnedPatch() || m_PatchFallbackWorld)
            return m_PreparedWorld;

        // 일반 메시와 강체 부착 조각은 기존 TRS 배치를 사용한다.
        return m_TransformMode == MeshTransformMode::Attached
            ? owner.ToAffine() * m_Transform.ToAffine() : m_Transform.ToAffine();
    }

    // source 기준을 보정한 동일 월드 행렬로 선택 경계를 만든다.
    BoundingSphere MeshComponent::GetBoundingSphere(const SceneTransform& owner) const
    {
        if (m_SectionBounds.empty())
            return BoundingSphere(kZero);
        BoundingSphere result = m_SectionBounds.front();
        for (size_t i = 1; i < m_SectionBounds.size(); ++i)
            result = result.Union(m_SectionBounds[i]);
        return TransformBoundingSphere(GetWorldTransform(owner), result);
    }

    // PG를 만든 section 행렬만 복사해 이후 node 애니메이션과 분리한다.
    bool MeshComponent::BuildCutSectionFrames(std::vector<Matrix4>& frames) const
    {
        // 준비되지 않은 pose나 이전 revision의 행렬은 제출하지 않는다.
        if (!HasPoseBounds() || m_CutSectionFrames.size() != m_Geometry->GetSections().size())
            return false;

        // 생성 revision에는 Update에서 기록한 단위 행렬을 사용한다.
        frames = m_CutSectionFrames;
        return true;
    }

    // 실제 게시 시 skin 입력으로 전용 PG를 교체하고 재사용할 팔레트를 한 번 할당한다.
    void MeshComponent::ActivatePatchSkin()
    {
        // 스키닝을 허용하지 않은 조각은 기존 강체 경로를 유지한다.
        if (!PatchSkin || !PatchSkin->Snapshot.SkinningAllowed)
            return;
        if (!m_Geometry->IsGenerated() || !Attachment || PatchSkin->Snapshot.MeshNode == UINT32_MAX)
            throw std::runtime_error("Invalid patch skin publication");

        // section 순서를 렌더 offset과 일치시키며 게시 전 PG를 할당한다.
        std::vector<PosedGeometry::VertexRange> sections;
        for (const auto& section : PatchSkin->Sections)
            sections.push_back({ section.FirstVertex, section.VertexCount });
        auto posed = std::make_unique<PosedGeometry>(PatchSkin->Vertices.Data,
            PatchSkin->Vertices.Stride, PatchSkin->Vertices.Flags, sections);
        m_PosedGeometry = std::move(posed);

        // 고정 joint 수와 강체 슬롯에 맞춰 팔레트를 준비하고 게시 월드를 기록한다.
        m_PatchPalette.resize(PatchSkin->Snapshot.JointsAtCut.size() + 1);
        const Matrix4& world = Attachment->PieceWorldAtCut;
        m_PreparedWorld = AffineTransform((Vector3)world.GetX(), (Vector3)world.GetY(),
            (Vector3)world.GetZ(), (Vector3)world.GetW());
        m_PatchSkinActive = true;
        m_PatchFallbackWorld = false;
        m_Ready = false;
    }

    // 후보 시점의 PG를 불변 Data revision으로 게시하고 실패 복구 없는 강체 부착으로 전환한다.
    bool MeshComponent::BakePatchSkin(const Matrix4& nodeWorld)
    {
        // 활성 조각의 준비된 PG와 손실 없는 강체 배치만 허용한다.
        if (!HasSkinnedPatch())
            return true;
        SceneTransform world;
        if (!Attachment || !m_Ready || !m_PosedGeometry->IsReady()
            || !SceneTransform::TryFromAffine(AffineTransform(nodeWorld * Invert(nodeWorld)
                * Matrix4(m_PreparedWorld)), world))
            return false;

        // 기존 GPU 메타데이터와 보수적 현재 경계를 새 revision에 보존한다.
        const auto parent = m_Geometry->GetGenerated();
        auto buffers = std::make_shared<GeometryBuffers>();
        buffers->Source = parent->Source;
        buffers->Parent = m_Geometry;
        buffers->Generation = parent->Generation;
        buffers->UncappedSegments = parent->UncappedSegments;
        buffers->Meshes = parent->Meshes;
        for (size_t s = 0; s < m_Geometry->GetSections().size(); ++s)
        {
            const auto& bound = m_SectionBounds.at(s);
            auto& mesh = buffers->Meshes.at(m_Geometry->GetSections()[s].SourceMesh);
            DirectX::XMStoreFloat4(reinterpret_cast<DirectX::XMFLOAT4*>(mesh.bounds),
                Vector4(bound.GetCenter(), bound.GetRadius()));
        }
        ByteAddressBuffer* targets[] = { &buffers->Data, &buffers->Provenance, &buffers->CapMaterial };
        const ByteAddressBuffer* sources[] = { &parent->Data, &parent->Provenance, &parent->CapMaterial };
        uint64_t bytes = 0;
        for (uint32_t i = 0; i < 3; ++i)
        {
            const uint32_t size = uint32_t(sources[i]->GetBufferSize());
            if (size)
                targets[i]->Create(L"PS6 baked geometry", size, 1);
            bytes += size;
        }
        buffers->LineageBytes = parent->LineageBytes + bytes;
        const auto revision = GeometryRevision::FromGenerated(buffers);
        auto posed = std::make_unique<PosedGeometry>(*buffers);

        // 재사용 파이프라인은 RAW40 입력과 복사한 28 B Data만 바인딩한다.
        static RootSignature root;
        static ComputePSO pso(L"Bake patch skin");
        static bool initialized = false;
        if (!initialized)
        {
            root.Reset(3);
            root[0].InitAsConstants(0, 5);
            root[1].InitAsBufferSRV(0);
            root[2].InitAsBufferUAV(0);
            root.Finalize(L"Patch skin bake");
            pso.SetRootSignature(root);
            pso.SetComputeShader(g_pPatchSkinBakeCS, sizeof(g_pPatchSkinBakeCS));
            pso.Finalize();
            initialized = true;
        }

        // 전체 Data 복사 뒤 현재 PG 속성만 덮어쓰고 VB 입력 상태를 복원한다.
        auto& context = GraphicsContext::Begin(L"PS6 bake");
        for (uint32_t i = 0; i < 3; ++i)
        {
            if (!sources[i]->GetBufferSize())
                continue;
            context.CopyBuffer(*targets[i], const_cast<ByteAddressBuffer&>(*sources[i]));
            context.TransitionResource(const_cast<ByteAddressBuffer&>(*sources[i]), D3D12_RESOURCE_STATE_GENERIC_READ);
            context.TransitionResource(*targets[i], D3D12_RESOURCE_STATE_GENERIC_READ);
        }
        auto& raw = const_cast<ByteAddressBuffer&>(m_PosedGeometry->GetBuffer());
        auto& compute = context.GetComputeContext();
        compute.SetRootSignature(root);
        compute.SetPipelineState(pso);
        compute.TransitionResource(raw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        compute.TransitionResource(buffers->Data, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        compute.SetBufferSRV(1, raw);
        compute.SetBufferUAV(2, buffers->Data);
        for (const auto& section : m_Geometry->GetSections())
        {
            const auto& mesh = buffers->Meshes[section.SourceMesh];
            const uint32_t count = mesh.vbSize / 28;
            const uint32_t constants[] = { m_PosedGeometry->GetOffset(section.SourceMesh),
                mesh.vbOffset, mesh.vbDepthOffset, count, (mesh.psoFlags & PSOFlags::kAlphaTest) ? 16u : 12u };
            compute.SetConstantArray(0, 5, constants);
            if (!count)
                continue;
            const uint32_t groups = (count + 63) / 64;
            compute.Dispatch(std::min(groups, 65535u), (groups + 65534) / 65535);
        }
        context.TransitionResource(buffers->Data, D3D12_RESOURCE_STATE_GENERIC_READ);
        context.TransitionResource(raw, D3D12_RESOURCE_STATE_GENERIC_READ);
        posed->UpdateGenerated(context, *buffers);

        // 진단 옵션에서만 동일 프레임의 베이크 결과와 원래 PG를 동기 비교한다.
        uint32_t rigChecks = 0;
        uint32_t recutChecks = 0;
        CommandLineArgs::GetInteger(L"rig-bone-cut-checks", rigChecks);
        CommandLineArgs::GetInteger(L"attached-recut-checks", recutChecks);
        ReadbackBuffer bakedReadback;
        ReadbackBuffer rawReadback;
        if (rigChecks || recutChecks)
        {
            bakedReadback.Create(L"PS6 Data readback", uint32_t(buffers->Data.GetBufferSize()), 1);
            rawReadback.Create(L"PS6 PG readback", uint32_t(raw.GetBufferSize()), 1);
            context.CopyBuffer(bakedReadback, buffers->Data);
            context.CopyBuffer(rawReadback, raw);
            context.TransitionResource(buffers->Data, D3D12_RESOURCE_STATE_GENERIC_READ);
            context.TransitionResource(raw, D3D12_RESOURCE_STATE_GENERIC_READ);
        }
        context.Finish(true);

        // 강체 경로의 재구성 월드와 같은 프레임 원본 월드로 모든 위치를 비교한다.
        if (rigChecks || recutChecks)
        {
            const auto* baked = static_cast<const uint8_t*>(bakedReadback.Map());
            const auto* original = static_cast<const uint8_t*>(rawReadback.Map());
            const Matrix4 rigidWorld(world.ToAffine());
            const Matrix4 skinWorld(m_PreparedWorld);
            double maxError = 0.0;
            uint32_t vertices = 0;
            for (const auto& section : m_Geometry->GetSections())
            {
                const auto& mesh = buffers->Meshes[section.SourceMesh];
                for (uint32_t v = 0; v < mesh.vbSize / 28; ++v)
                {
                    DirectX::XMFLOAT3 a;
                    DirectX::XMFLOAT3 b;
                    std::memcpy(&a, baked + mesh.vbOffset + v * 28, sizeof(a));
                    std::memcpy(&b, original + m_PosedGeometry->GetOffset(section.SourceMesh) + v * 40, sizeof(b));
                    const float error = float(Length(Vector3(rigidWorld * Vector4(Vector3(a), 1))
                        - Vector3(skinWorld * Vector4(Vector3(b), 1)))) * 1000.0f;
                    maxError = std::isfinite(error) ? std::max(maxError, double(error)) : INFINITY;
                    ++vertices;
                }
            }
            bakedReadback.Unmap();
            rawReadback.Unmap();
            if (!vertices)
                maxError = INFINITY;
            char line[256];
            sprintf_s(line, "PS6 bake side=%s vertices=%u maxError_mm=%.6f result=%s\n",
                PatchSkin->Side, vertices, maxError, maxError <= 0.01 ? "PASS" : "FAIL");
            Utility::Print(line);
            char path[MAX_PATH];
            FILE* file = nullptr;
            if (GetTempPathA(MAX_PATH, path))
            {
                strcat_s(path, "SP6-SkeletalPatchChecks.log");
                if (!fopen_s(&file, path, "a"))
                {
                    fputs(line, file);
                    fclose(file);
                }
            }
        }

        // 제출 완료된 새 PG를 게시하고 기존 skin 입력은 다음 프레임까지 보관한다.
        Attachment->NodeWorldAtCut = nodeWorld;
        Attachment->PieceWorldAtCut = Matrix4(m_PreparedWorld);
        SetAttachedWorld(world);
        m_PatchSkinActive = false;
        m_RetiredPatchSkin = std::move(PatchSkin);
        m_RetiredPatchPose = std::move(m_PosedGeometry);
        m_PosedGeometry = std::move(posed);
        m_Geometry = revision;
        ++m_EditGeneration;
        m_Ready = true;
        SetInheritedCollision({}, revision->GetId(), "node attached");
        return true;
    }

    // 원본의 revision과 binding을 검사하고 불일치는 현재 월드를 기준으로 강체 부착으로 전환한다.
    void MeshComponent::ValidatePatchSkinSource(const MeshComponent* source, const Matrix4* nodeWorld)
    {
        // 활성 조각만 snapshot과 비교하며 일시적인 pose 준비 실패는 비활성화하지 않는다.
        if (!HasSkinnedPatch())
            return;
        const auto& snapshot = PatchSkin->Snapshot;
        if (source && !source->GetGeometry()->IsGenerated()
            && source->GetGeometry()->GetSource() == m_Geometry->GetSource()
            && source->GetGeometry()->GetId() == snapshot.SourceRevision)
        {
            const auto& binding = source->GetDeformBinding();
            const Matrix4 frame(binding.SourceFrame.ToAffine());
            if (std::memcmp(&frame, &snapshot.SourceFrame, sizeof(frame)) == 0
                && (binding.Pose == PoseMode::Frozen) == snapshot.SourceFrozen)
                return;
        }

        // 기존 Data 강체 경로로 복귀하며 현재 월드와 현재 부착 노드를 새 기준으로 잡는다.
        auto posed = std::make_unique<PosedGeometry>(*m_Geometry->GetGenerated());
        SceneTransform world;
        const bool hasWorldTRS = SceneTransform::TryFromAffine(m_PreparedWorld, world);
        if (Attachment && nodeWorld)
        {
            Attachment->NodeWorldAtCut = *nodeWorld;
            Attachment->PieceWorldAtCut = Matrix4(m_PreparedWorld);
        }
        m_PatchSkinActive = false;
        ++m_EditGeneration;
        m_PosedGeometry = std::move(posed);
        if (hasWorldTRS)
            SetAttachedWorld(world);
        else
        {
            // 전단이 있는 월드는 TRS로 손실 변환하지 않고 마지막 정확한 W를 유지한다.
            m_PatchFallbackWorld = true;
            m_TransformMode = MeshTransformMode::WorldIndependent;
            m_Ready = false;
        }

        // 활성 플래그를 내렸으므로 같은 조각의 호환성 실패 로그는 한 번만 나온다.
        Utility::Printf("Patch skin deactivated: source revision/binding changed mesh=%llu\n",
            static_cast<unsigned long long>(m_Id));
    }

    // 확정 CPU pose와 재사용 팔레트로 PG를 갱신하고 스키닝 조각에 원본의 W와 N을 적용한다.
    void MeshComponent::Update(GraphicsContext& context, const ModelInstance& pose, const SceneTransform& owner,
        const MeshComponent* patchSource)
    {
        // 이전 프레임까지 제출된 skin 입력은 queue fence를 통해 지연 해제한다.
        if (m_RetiredPatchSkin)
        {
            Graphics::g_CommandManager.RetireResource(m_RetiredPatchSkin->Vertices.Data.GetResource());
            m_RetiredPatchSkin.reset();
            m_RetiredPatchPose.reset();
            m_PatchPalette.clear();
        }

        // 같은 명령 목록에서 마스크를 먼저 갱신한다.
        UpdateTriangleMask(context);

        // 최종 pose의 유효성을 검사한다.
        m_Ready = false;
        m_RenderNodes = nullptr;
        m_RenderJoints = nullptr;
        m_CutSectionFrames.clear();
        if (!pose.HasValidPose() || pose.GetModel() != m_Geometry->GetSource())
            return;

        // 본 부착 조각은 초록색이며 원본과 Static 절단 조각은 흰색이다.
        const float overlayColor[3] = { Attachment ? 0.25f : 1.0f, 1.0f, Attachment ? 0.35f : 1.0f };

        // 생성 조각의 월드 상수를 업로드하고 Data 디코드 결과를 렌더 입력으로 준비한다.
        if (m_Geometry->IsGenerated())
        {
            // 스키닝 조각은 부착 월드 대신 원본의 현재 월드와 최종 pose를 사용한다.
            if (HasSkinnedPatch())
            {
                if (!patchSource || !patchSource->GetPreparedNodes() || !patchSource->GetPreparedJoints()
                    || PatchSkin->Snapshot.MeshNode >= pose.GetModel()->m_NumNodes)
                    return;
                BuildPatchPalette(PatchSkin->Snapshot, patchSource->GetPreparedJoints(), patchSource->GetPreparedNodes(), m_PatchPalette);
                const Matrix4 sourceToComponent(Invert(patchSource->GetDeformBinding().SourceFrame.ToAffine()));
                const Matrix4 node = sourceToComponent * patchSource->GetPreparedNodes()[PatchSkin->Snapshot.MeshNode];
                m_PosedGeometry->UpdateSkinnedGenerated(context, PatchSkin->Vertices.Data, m_PatchPalette, node);
                m_PreparedWorld = patchSource->GetWorldTransform(owner);

                // 원본과 같이 section bind 구를 전체 팔레트로 변환·합산하고 zero-weight 원점도 포함한다.
                m_SectionBounds.clear();
                for (const auto& section : m_Geometry->GetSections())
                {
                    const auto& rest = PatchSkin->Vertices.BindBounds.at(section.SourceMesh);
                    BoundingSphere bound(kZero);
                    for (const auto& joint : m_PatchPalette)
                    {
                        const Matrix4 skin = node * joint.posXform;
                        const AffineTransform affine((Vector3)skin.GetX(), (Vector3)skin.GetY(),
                            (Vector3)skin.GetZ(), (Vector3)skin.GetW());
                        bound = bound.Union(TransformBoundingSphere(affine, rest));
                    }
                    m_SectionBounds.push_back(bound);
                }
            }
            else
            {
                // 일반 생성 조각은 기존 Data 디코드와 자체 배치를 유지한다.
                m_PosedGeometry->UpdateGenerated(context, *m_Geometry->GetGenerated());
                m_PreparedWorld = GetWorldTransform(owner);
            }

            // 모든 생성 조각은 PG 위치에 렌더 월드를 한 번만 곱한다.
            const Matrix4 world(m_PreparedWorld);
            MeshConstants cb{};
            cb.World = cb.ObjectWorld = world;
            cb.WorldIT = cb.ObjectWorldIT = InverseTranspose(world.Get3x3());
            cb.UsePosedVertices = 1;
            std::memcpy(cb.OverlayColor, overlayColor, sizeof(overlayColor));
            auto* mapped = static_cast<MeshConstants*>(m_ConstantsCPU.Map());
            mapped[0] = cb;
            m_ConstantsCPU.Unmap();
            context.TransitionResource(m_ConstantsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
            context.GetCommandList()->CopyBufferRegion(m_ConstantsGPU.GetResource(), 0,
                m_ConstantsCPU.GetResource(), 0, sizeof(cb));
            context.TransitionResource(m_ConstantsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);

            // 일반 생성 조각만 Data 경계를 쓰며 스키닝 조각은 위에서 계산한 현재 경계를 유지한다.
            if (!HasSkinnedPatch())
            {
                m_SectionBounds.clear();
                for (size_t i = 0; i < m_Geometry->GetSections().size(); ++i)
                    m_SectionBounds.emplace_back(reinterpret_cast<const XMFLOAT4*>(m_Geometry->GetMesh(i).bounds));
            }

            // 생성 데이터는 component-local이므로 node 변환을 다시 적용하지 않는다.
            m_CutSectionFrames.assign(m_Geometry->GetSections().size(), Matrix4(kIdentity));
            m_RenderJoints = nullptr;
            m_Ready = true;
            return;
        }

        // 원본 geometry는 공유 pose 또는 고정 snapshot을 사용한다.
        const Model& model = *pose.GetModel();
        const Matrix4* nodes = pose.GetNodeTransforms();
        const Joint* joints = pose.GetJoints();
        if (m_DeformBinding.Pose == PoseMode::Frozen)
        {
            if (m_FrozenNodes.empty())
            {
                m_FrozenNodes.assign(nodes, nodes + model.m_NumNodes);
                if (model.m_NumJoints > 0)
                    m_FrozenJoints.assign(joints, joints + model.m_NumJoints);
            }
            nodes = m_FrozenNodes.data();
            joints = m_FrozenJoints.data();
        }

        // Frozen 선택을 마친 node와 source 기준으로 PG 및 절단 행렬을 함께 만든다.
        m_RenderJoints = joints;
        m_RenderNodes = nodes;
        const Matrix4 sourceToComponent(Invert(m_DeformBinding.SourceFrame.ToAffine()));
        m_PreparedWorld = GetWorldTransform(owner);
        const Matrix4 world(m_PreparedWorld);
        std::vector<Matrix4> localNodes(model.m_NumNodes);
        MeshConstants* constants = static_cast<MeshConstants*>(m_ConstantsCPU.Map());
        for (uint32_t i = 0; i < model.m_NumNodes; ++i)
        {
            localNodes[i] = sourceToComponent * nodes[i];
            const Matrix4 nodeWorld = world * localNodes[i];
            constants[i].World = nodeWorld;
            constants[i].WorldIT = InverseTranspose(nodeWorld.Get3x3());
            constants[i].ObjectWorld = world;
            constants[i].ObjectWorldIT = InverseTranspose(world.Get3x3());
            std::memcpy(constants[i].OverlayColor, overlayColor, sizeof(overlayColor));
        }

        // 비음수 정규화 LBS는 본별 변환 구의 convex hull 안에 있다. 전체 팔레트 union은 느슨하지만 누락을 막는다.
        m_SectionBounds.clear();
        for (size_t i = 0; i < m_Geometry->GetSections().size(); ++i)
        {
            const Mesh& mesh = m_Geometry->GetMesh(i);
            const BoundingSphere rest(reinterpret_cast<const XMFLOAT4*>(mesh.bounds));
            const Matrix4& node = localNodes[mesh.meshCBV];
            m_CutSectionFrames.push_back(node);
            const AffineTransform nodeAffine((Vector3)node.GetX(), (Vector3)node.GetY(),
                (Vector3)node.GetZ(), (Vector3)node.GetW());
            BoundingSphere bound = TransformBoundingSphere(nodeAffine, rest);
            if (mesh.psoFlags & PSOFlags::kHasSkin)
            {
                bound = BoundingSphere(kZero); // weight 합 0인 입력의 shader 결과도 포함한다.
                for (uint32_t j = 0; j < mesh.numJoints; ++j)
                {
                    const Matrix4 skin = node * joints[mesh.startJoint + j].posXform;
                    const AffineTransform affine((Vector3)skin.GetX(), (Vector3)skin.GetY(),
                        (Vector3)skin.GetZ(), (Vector3)skin.GetW());
                    bound = bound.Union(TransformBoundingSphere(affine, rest));
                }
            }
            m_SectionBounds.push_back(bound);
        }

        // 공통 pose 준비 결과와 메시 상수를 GPU에 반영한다.
        if (Renderer::UseComputePose)
            m_PosedGeometry->Update(context, model, localNodes.data(), joints);
        for (uint32_t i = 0; i < model.m_NumNodes; ++i)
            constants[i].UsePosedVertices = Renderer::UseComputePose && m_PosedGeometry->IsReady();
        m_ConstantsCPU.Unmap();
        context.TransitionResource(m_ConstantsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
        context.GetCommandList()->CopyBufferRegion(m_ConstantsGPU.GetResource(), 0,
            m_ConstantsCPU.GetResource(), 0, m_ConstantsCPU.GetBufferSize());
        context.TransitionResource(m_ConstantsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);
        m_Ready = true;
    }

    // 같은 revision의 버퍼·재질·출처를 사용하고 section마다 보수적 구 컬링을 수행한다.
    void MeshComponent::Render(Renderer::MeshSorter& sorter) const
    {
        // break 성공 전의 준비물은 렌더 목록에 넣지 않는다.
        if (!m_Ready || !PublishVisible)
            return;
        const auto& sections = m_Geometry->GetSections();
        const Matrix4& viewMatrix = sorter.GetViewMatrix();
        const AffineTransform view((Vector3)viewMatrix.GetX(), (Vector3)viewMatrix.GetY(),
            (Vector3)viewMatrix.GetZ(), (Vector3)viewMatrix.GetW());
        const bool posed = (Renderer::UseComputePose || m_Geometry->IsGenerated()) && m_PosedGeometry->IsReady();
        const Joint* joints = m_RenderJoints;
        for (size_t i = 0; i < sections.size(); ++i)
        {
            const Mesh& mesh = m_Geometry->GetMesh(i);
            const BoundingSphere sphere = TransformBoundingSphere(m_PreparedWorld, m_SectionBounds[i]);
            const BoundingSphere sphereVS(view * sphere.GetCenter(), sphere.GetRadius());
            if (!sorter.GetViewFrustum().IntersectSphere(sphereVS))
                continue;
            sorter.AddMesh(mesh, -sphereVS.GetCenter().GetZ() - sphereVS.GetRadius(),
                m_ConstantsGPU.GetGpuVirtualAddress() + sizeof(MeshConstants) * mesh.meshCBV,
                m_Geometry->GetMaterialAddress(mesh),
                m_Geometry->GetDataBuffer().GetGpuVirtualAddress(), joints,
                posed ? &m_PosedGeometry->GetBuffer() : nullptr,
                posed ? m_PosedGeometry->GetOffset(sections[i].SourceMesh) : 0,
                m_TriangleMask && m_TriangleMask->AppliedRevision
                    ? m_TriangleMask->ReplacedIB.GetGpuVirtualAddress() - m_TriangleMask->FirstByte : 0);
        }
    }
}
