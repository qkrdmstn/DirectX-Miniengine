//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author:   James Stanard
//

#include "Model.h"
#include "Renderer.h"
#include "ConstantBuffers.h"

using namespace Math;
using namespace Renderer;

void Model::Destroy()
{
    m_BoundingSphere = BoundingSphere(kZero);
    m_DataBuffer.Destroy();
    m_MaterialConstants.Destroy();
    m_NumNodes = 0;
    m_NumMeshes = 0;
    m_MeshData = nullptr;
    m_SceneGraph = nullptr;
}

// affine 경계 구를 뷰 공간으로 옮겨 컬링하고 정렬할 메시를 등록한다.
void Model::Render(
    MeshSorter& sorter,
    const GpuBuffer& meshConstants,
    const AffineTransform sphereTransforms[],
    const Joint* skeleton ) const
{
    // Pointer to current mesh
    const uint8_t* pMesh = m_MeshData.get();

    const Frustum& frustum = sorter.GetViewFrustum();
    const AffineTransform& viewMat = (const AffineTransform&)sorter.GetViewMatrix();

    for (uint32_t i = 0; i < m_NumMeshes; ++i)
    {
        const Mesh& mesh = *(const Mesh*)pMesh;

        const AffineTransform& sphereXform = sphereTransforms[mesh.meshCBV];
        const BoundingSphere sphereLS((const XMFLOAT4*)mesh.bounds);
        const BoundingSphere sphereWS = TransformBoundingSphere(sphereXform, sphereLS);
        BoundingSphere sphereVS = BoundingSphere(viewMat * sphereWS.GetCenter(), sphereWS.GetRadius());

        if (frustum.IntersectSphere(sphereVS))
        {
            float distance = -sphereVS.GetCenter().GetZ() - sphereVS.GetRadius();
            sorter.AddMesh(mesh, distance,
                meshConstants.GetGpuVirtualAddress() + sizeof(MeshConstants) * mesh.meshCBV,
                m_MaterialConstants.GetGpuVirtualAddress() + sizeof(MaterialConstants) * mesh.materialCBV,
                m_DataBuffer.GetGpuVirtualAddress(), skeleton);
        }

        pMesh += sizeof(Mesh) + (mesh.numDraws - 1) * sizeof(Mesh::Draw);
    }
}

void ModelInstance::Render(MeshSorter& sorter) const
{
    if (m_Model != nullptr)
    {
        //const Frustum& frustum = sorter.GetWorldFrustum();
        m_Model->Render(sorter, m_MeshConstantsGPU, m_BoundingSphereTransforms.get(),
            m_Skeleton.get());
    }
}

ModelInstance::ModelInstance( std::shared_ptr<const Model> sourceModel )
    : m_Model(sourceModel), m_Locator(kIdentity)
{
    static_assert((_alignof(MeshConstants) & 255) == 0, "CBVs need 256 byte alignment");
    if (sourceModel == nullptr)
    {
        m_MeshConstantsCPU.Destroy();
        m_MeshConstantsGPU.Destroy();
        m_BoundingSphereTransforms = nullptr;
        m_ModelSpaceTransforms = nullptr;
        m_AnimGraph = nullptr;
        m_AnimState.clear();
        m_Skeleton = nullptr;
    }
    else
    {
        m_MeshConstantsCPU.Create(L"Mesh Constant Upload Buffer", sourceModel->m_NumNodes * sizeof(MeshConstants));
        m_MeshConstantsGPU.Create(L"Mesh Constant GPU Buffer", sourceModel->m_NumNodes, sizeof(MeshConstants));

        m_BoundingSphereTransforms.reset(new AffineTransform[sourceModel->m_NumNodes]);
        m_ModelSpaceTransforms.reset(new Matrix4[sourceModel->m_NumNodes]);
        m_Skeleton.reset(new Joint[sourceModel->m_NumJoints]);

        if (sourceModel->m_NumAnimations > 0)
        {
            m_AnimGraph.reset(new GraphNode[sourceModel->m_NumNodes]);
            std::memcpy(m_AnimGraph.get(), sourceModel->m_SceneGraph.get(), sourceModel->m_NumNodes * sizeof(GraphNode));
            m_AnimState.resize(sourceModel->m_NumAnimations);
        }
        else
        {
            m_AnimGraph = nullptr;
            m_AnimState.clear();
        }
    }
}

ModelInstance::ModelInstance( const ModelInstance& modelInstance )
    : ModelInstance(modelInstance.m_Model)
{
}

// 모델을 교체하고 affine locator와 인스턴스 자원을 초기화한다.
ModelInstance& ModelInstance::operator=( std::shared_ptr<const Model> sourceModel )
{
    m_Model = sourceModel;
    m_Locator = AffineTransform(kIdentity);
    if (sourceModel == nullptr)
    {
        m_MeshConstantsCPU.Destroy();
        m_MeshConstantsGPU.Destroy();
        m_BoundingSphereTransforms = nullptr;
        m_ModelSpaceTransforms = nullptr;
        m_AnimGraph = nullptr;
        m_AnimState.clear();
        m_Skeleton = nullptr;
    }
    else
    {
        m_MeshConstantsCPU.Create(L"Mesh Constant Upload Buffer", sourceModel->m_NumNodes * sizeof(MeshConstants));
        m_MeshConstantsGPU.Create(L"Mesh Constant GPU Buffer", sourceModel->m_NumNodes, sizeof(MeshConstants));

        m_BoundingSphereTransforms.reset(new AffineTransform[sourceModel->m_NumNodes]);
        m_ModelSpaceTransforms.reset(new Matrix4[sourceModel->m_NumNodes]);
        m_Skeleton.reset(new Joint[sourceModel->m_NumJoints]);

        if (sourceModel->m_NumAnimations > 0)
        {
            m_AnimGraph.reset(new GraphNode[sourceModel->m_NumNodes]);
            std::memcpy(m_AnimGraph.get(), sourceModel->m_SceneGraph.get(), sourceModel->m_NumNodes * sizeof(GraphNode));
            m_AnimState.resize(sourceModel->m_NumAnimations);
        }
        else
        {
            m_AnimGraph = nullptr;
            m_AnimState.clear();
        }
    }
    return *this;
}

void ModelInstance::Update(GraphicsContext& gfxContext, float deltaTime)
{
    if (m_Model == nullptr)
        return;

    const Matrix4 LocatorMatrix((AffineTransform)m_Locator);

    MeshConstants* cb = (MeshConstants*)m_MeshConstantsCPU.Map();

    if (m_AnimGraph)
    {
        UpdateAnimations(deltaTime);

        for (uint32_t i = 0; i < m_Model->m_NumNodes; ++i)
        {
            GraphNode& node = m_AnimGraph[i];

            // Regenerate the 3x3 matrix if it has scale or rotation
            if (node.staleMatrix)
            {
                node.staleMatrix = false;
                node.xform.Set3x3(Matrix3(node.rotation) * Matrix3::MakeScale(node.scale));
            }
        }
    }

    const GraphNode* sceneGraph = m_AnimGraph ? m_AnimGraph.get() : m_Model->m_SceneGraph.get();

    if (!EvaluateModelPose(sceneGraph, m_Model->m_NumNodes, m_ModelSpaceTransforms.get()))
    {
        m_MeshConstantsCPU.Unmap();
        ASSERT(false, "Invalid model scene graph");
        return;
    }

    // Skinning happens before the shader's mesh World transform.  Keep the pose
    // model-local and apply the instance locator only to mesh/culling constants.
    for (uint32_t i = 0; i < m_Model->m_NumNodes; ++i)
    {
        const Matrix4 worldXform = LocatorMatrix * m_ModelSpaceTransforms[i];
        cb[i].World = worldXform;
        cb[i].WorldIT = InverseTranspose(worldXform.Get3x3());
        m_BoundingSphereTransforms[i] = AffineTransform(
            (Vector3)worldXform.GetX(), (Vector3)worldXform.GetY(),
            (Vector3)worldXform.GetZ(), (Vector3)worldXform.GetW());
    }

    // Update skeletal joints
    for (uint32_t i = 0; i < m_Model->m_NumJoints; ++i)
    {
        Joint& joint = m_Skeleton[i];
        joint.posXform = m_ModelSpaceTransforms[m_Model->m_JointIndices[i]] * m_Model->m_JointIBMs[i];
        joint.nrmXform = InverseTranspose(joint.posXform.Get3x3());
    }

    m_MeshConstantsCPU.Unmap();

    gfxContext.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_COPY_DEST, true);
    gfxContext.GetCommandList()->CopyBufferRegion(m_MeshConstantsGPU.GetResource(), 0, m_MeshConstantsCPU.GetResource(), 0, m_MeshConstantsCPU.GetBufferSize());
    gfxContext.TransitionResource(m_MeshConstantsGPU, D3D12_RESOURCE_STATE_GENERIC_READ);
}

// 위치와 축 비율을 유지하며 요청한 경계 반지름에 맞춘다.
void ModelInstance::Resize( float newRadius )
{
    if (m_Model == nullptr)
        return;

    const float radius = GetRadius();
    if (std::isfinite(newRadius) && newRadius > 0.0f && radius > 0.0f)
        m_Locator.SetBasis(m_Locator.GetBasis() * (newRadius / radius));
}

Vector3 ModelInstance::GetCenter() const
{
    if (m_Model == nullptr)
        return Vector3(kOrigin);

    return m_Locator * m_Model->m_BoundingSphere.GetCenter();
}

// affine 신장 상한을 반영한 월드 반지름을 반환한다.
Scalar ModelInstance::GetRadius() const
{
    if (m_Model == nullptr)
        return Scalar(kZero);

    return AffineRadiusScale(m_Locator) * m_Model->m_BoundingSphere.GetRadius();
}

// 전체 affine으로 중심과 보수적인 반지름을 변환한다.
Math::BoundingSphere ModelInstance::GetBoundingSphere() const
{
    if (m_Model == nullptr)
        return BoundingSphere(kZero);

    return TransformBoundingSphere(m_Locator, m_Model->m_BoundingSphere);
}

// affine locator로 원본 상자의 모서리와 기저를 변환한다.
Math::OrientedBox ModelInstance::GetBoundingBox() const
{
    if (m_Model == nullptr)
        return AxisAlignedBox(Vector3(kZero), Vector3(kZero));

    return m_Locator * OrientedBox(m_Model->m_BoundingBox);
}
