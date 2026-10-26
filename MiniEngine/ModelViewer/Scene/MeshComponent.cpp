#include "MeshComponent.h"
#include "ConstantBuffers.h"
#include "Renderer.h"
#include "CommandListManager.h"
#include "GraphicsCore.h"
#include "Util/CommandLineArg.h"
#include <cmath>
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
            m_CollisionReason.clear();
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
        SceneTransform normalized = transform;
        if (!normalized.Normalize())
            return false;
        ++m_StateVersion;
        m_Transform = normalized;
        m_TransformMode = mode;
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
        ++m_StateVersion;
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

        // revision을 교체하고 경계·준비 상태를 무효화한다.
        ++m_StateVersion;
        m_Geometry = std::move(geometry);
        m_SectionBounds.clear();
        m_Ready = false;

        // 이전 revision의 hull을 버리고 새 revision의 충돌을 만든다.
        RebuildCollision();
        return true;
    }

    // 소유 객체와의 종속 모드에서만 owner Transform을 곱한다.
    AffineTransform MeshComponent::GetWorldTransform(const SceneTransform& owner) const
    {
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

    // CPU pose는 복제 평가하지 않고 source-to-component와 target 월드 변환을 분리한다.
    void MeshComponent::Update(GraphicsContext& context, const ModelInstance& pose, const SceneTransform& owner)
    {
        m_Ready = false;
        if (!pose.HasValidPose() || pose.GetModel() != m_Geometry->GetSource())
            return;
        if (m_Geometry->IsGenerated())
        {
            m_PreparedWorld = GetWorldTransform(owner);
            const Matrix4 world(m_PreparedWorld);
            MeshConstants cb{};
            cb.World = cb.ObjectWorld = world;
            cb.WorldIT = cb.ObjectWorldIT = InverseTranspose(world.Get3x3());
            cb.UsePosedVertices = 1;
            auto* mapped = static_cast<MeshConstants*>(m_ConstantsCPU.Map());
            mapped[0] = cb;
            m_ConstantsCPU.Unmap();
            context.TransitionResource(m_ConstantsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
            context.GetCommandList()->CopyBufferRegion(m_ConstantsGPU.GetResource(), 0,
                m_ConstantsCPU.GetResource(), 0, sizeof(cb));
            context.TransitionResource(m_ConstantsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);
            m_PosedGeometry->UpdateBaked(context, *m_Geometry->GetGenerated());
            m_SectionBounds.clear();
            for (size_t i = 0; i < m_Geometry->GetSections().size(); ++i)
                m_SectionBounds.emplace_back(reinterpret_cast<const XMFLOAT4*>(m_Geometry->GetMesh(i).bounds));
            m_RenderJoints = nullptr;
            m_Ready = true;
            return;
        }
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

        m_RenderJoints = joints;
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

        }
        // 비음수 정규화 LBS는 본별 변환 구의 convex hull 안에 있다. 전체 팔레트 union은 느슨하지만 누락을 막는다.
        m_SectionBounds.clear();
        for (size_t i = 0; i < m_Geometry->GetSections().size(); ++i)
        {
            const Mesh& mesh = m_Geometry->GetMesh(i);
            const BoundingSphere rest(reinterpret_cast<const XMFLOAT4*>(mesh.bounds));
            const Matrix4& node = localNodes[mesh.meshCBV];
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
        if (!m_Ready)
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
                posed ? m_PosedGeometry->GetOffset(sections[i].SourceMesh) : 0);
        }
    }
}
