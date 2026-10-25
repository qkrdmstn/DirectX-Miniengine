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

// 공유 원본을 보관하고 GameObject 하나의 CPU 애니메이션 상태를 준비한다.
ModelInstance::ModelInstance(std::shared_ptr<const Model> sourceModel)
    : m_Model(std::move(sourceModel))
{
    if (!m_Model)
        return;
    m_ModelSpaceTransforms = std::make_unique<Matrix4[]>(m_Model->m_NumNodes);
    m_Skeleton = std::make_unique<Joint[]>(m_Model->m_NumJoints);
    if (m_Model->m_NumAnimations > 0)
    {
        m_AnimGraph = std::make_unique<GraphNode[]>(m_Model->m_NumNodes);
        std::memcpy(m_AnimGraph.get(), m_Model->m_SceneGraph.get(), m_Model->m_NumNodes * sizeof(GraphNode));
        m_AnimState.resize(m_Model->m_NumAnimations);
    }
}

// 애니메이션과 model-local pose만 평가한다. GPU 준비는 각 MeshComponent가 수행한다.
void ModelInstance::Update(float deltaTime)
{
    m_PoseValid = false;
    if (!m_Model)
        return;
    if (m_AnimGraph)
    {
        UpdateAnimations(deltaTime);
        for (uint32_t i = 0; i < m_Model->m_NumNodes; ++i)
        {
            GraphNode& node = m_AnimGraph[i];
            if (node.staleMatrix)
            {
                node.staleMatrix = false;
                node.xform.Set3x3(Matrix3(node.rotation) * Matrix3::MakeScale(node.scale));
            }
        }
    }
    const GraphNode* graph = m_AnimGraph ? m_AnimGraph.get() : m_Model->m_SceneGraph.get();
    if (!EvaluateModelPose(graph, m_Model->m_NumNodes, m_ModelSpaceTransforms.get()))
    {
        ASSERT(false, "Invalid model scene graph");
        return;
    }
    for (uint32_t i = 0; i < m_Model->m_NumJoints; ++i)
    {
        Joint& joint = m_Skeleton[i];
        joint.posXform = m_ModelSpaceTransforms[m_Model->m_JointIndices[i]] * m_Model->m_JointIBMs[i];
        joint.nrmXform = InverseTranspose(joint.posXform.Get3x3());
    }
    m_PoseValid = true;
}
