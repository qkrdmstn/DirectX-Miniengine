#include "MeshComponent.h"
#include "ConstantBuffers.h"
#include "Renderer.h"
#include "CommandListManager.h"
#include "GraphicsCore.h"
#include <stdexcept>

using namespace Math;

namespace Viewer
{
    // 불변 원본 참조와 컴포넌트 전용 GPU 저장 공간을 준비한다.
    MeshComponent::MeshComponent(ComponentId id, std::shared_ptr<const GeometryRevision> geometry)
        : m_Id(id), m_Geometry(std::move(geometry))
    {
        if (id == InvalidComponentId || !m_Geometry)
            throw std::invalid_argument("Invalid mesh component");
        const Model& model = *m_Geometry->GetSource();
        if (model.m_NumNodes == 0)
            throw std::invalid_argument("Mesh component requires nodes");
        m_PosedGeometry = m_Geometry->IsGenerated() ? std::make_unique<PosedGeometry>(*m_Geometry->GetGenerated())
            : std::make_unique<PosedGeometry>(model);
        m_ConstantsCPU.Create(L"Component constants upload", model.m_NumNodes * sizeof(MeshConstants));
        m_ConstantsGPU.Create(L"Component constants", model.m_NumNodes, sizeof(MeshConstants));

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
        DeformBinding normalized = binding;
        if (!normalized.SourceFrame.Normalize())
            return false;
        ++m_StateVersion;
        m_DeformBinding = normalized;
        m_SectionBounds.clear();
        m_FrozenNodes.clear();
        m_FrozenJoints.clear();
        m_Ready = false;
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
        if (!geometry || geometry->GetSource() != m_Geometry->GetSource())
            return false;
        auto posed = geometry->IsGenerated() ? std::make_unique<PosedGeometry>(*geometry->GetGenerated())
            : std::make_unique<PosedGeometry>(*geometry->GetSource());
        m_PosedGeometry.reset();
        m_PosedGeometry = std::move(posed);
        ++m_StateVersion;
        m_Geometry = std::move(geometry);
        m_SectionBounds.clear();
        m_Ready = false;
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
