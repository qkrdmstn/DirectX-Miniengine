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
    // 원본이 없으면 비어 있는 인스턴스를 유지한다.
    if (!m_Model)
        return;

    // 직렬화 그래프를 바꾸지 않고 인스턴스별 출력과 유효 부모를 준비한다.
    m_ModelSpaceTransforms = std::make_unique<Matrix4[]>(m_Model->m_NumNodes);
    m_Skeleton = std::make_unique<Joint[]>(m_Model->m_NumJoints);
    BuildPoseParents(m_Model->m_SceneGraph.get(), m_Model->m_NumNodes, m_PoseParents);

    // 애니메이션이 있는 모델만 수정 가능한 local 그래프를 복사한다.
    if (m_Model->m_NumAnimations > 0)
    {
        m_AnimGraph = std::make_unique<GraphNode[]>(m_Model->m_NumNodes);
        std::memcpy(m_AnimGraph.get(), m_Model->m_SceneGraph.get(), m_Model->m_NumNodes * sizeof(GraphNode));
        m_AnimState.resize(m_Model->m_NumAnimations);
    }
}

// 애니메이션을 진행하고 물리 단계에서 읽을 model-local node pose를 평가한다.
void ModelInstance::EvaluateAnimation(float deltaTime)
{
    // 새 평가를 시작하면 이전 최종 pose와 팔레트의 유효성을 해제한다.
    m_PoseValid = false;
    m_AnimationValid = false;
    if (!m_Model || m_PoseParents.size() != m_Model->m_NumNodes)
        return;

    // 인스턴스 local 애니메이션과 변경된 회전·크기 행렬을 갱신한다.
    if (m_AnimGraph)
    {
        UpdateAnimations(deltaTime);
        for (uint32_t i = 0; i < m_Model->m_NumNodes; ++i)
        {
            // 애니메이션이 수정한 local 행렬만 다시 합성한다.
            GraphNode& node = m_AnimGraph[i];
            if (node.staleMatrix)
            {
                node.staleMatrix = false;
                node.xform.Set3x3(Matrix3(node.rotation) * Matrix3::MakeScale(node.scale));
            }
        }
    }

    // 기존 평가기와 같은 연산 순서로 물리 입력용 node pose를 공개한다.
    const GraphNode* graph = m_AnimGraph ? m_AnimGraph.get() : m_Model->m_SceneGraph.get();
    if (!EvaluateModelPose(graph, m_Model->m_NumNodes, m_ModelSpaceTransforms.get()))
    {
        ASSERT(false, "Invalid model scene graph");
        return;
    }
    m_AnimationValid = true;
}

// 물리 model pose를 합성하고 중복 node를 포함한 전체 본 팔레트를 확정한다.
bool ModelInstance::FinalizePose(const NodePhysicsPose* overrides, uint32_t overrideCount)
{
    // 애니메이션 평가 실패 또는 잘못된 팔레트 참조는 렌더에 공개하지 않는다.
    m_PoseValid = false;
    if (!m_Model || !m_AnimationValid)
        return false;
    for (uint32_t i = 0; i < m_Model->m_NumJoints; ++i)
    {
        // 모든 참조를 먼저 검사하여 팔레트를 부분 갱신하지 않는다.
        if (m_Model->m_JointIndices[i] >= m_Model->m_NumNodes)
            return false;
    }

    // local 애니메이션은 보존하고 최종 model pose만 override로 다시 평가한다.
    const GraphNode* graph = m_AnimGraph ? m_AnimGraph.get() : m_Model->m_SceneGraph.get();
    if (!EvaluatePoseOverrides(graph, m_Model->m_NumNodes, m_PoseParents,
        overrides, overrideCount, m_ModelSpaceTransforms.get()))
        return false;

    // node 식별자가 중복되어도 각 팔레트의 IBM과 normal 행렬을 따로 갱신한다.
    for (uint32_t i = 0; i < m_Model->m_NumJoints; ++i)
    {
        // 팔레트 인덱스 순서를 유지하여 여러 skin의 동일 본도 모두 반영한다.
        Joint& joint = m_Skeleton[i];
        joint.posXform = m_ModelSpaceTransforms[m_Model->m_JointIndices[i]] * m_Model->m_JointIBMs[i];
        joint.nrmXform = InverseTranspose(joint.posXform.Get3x3());
    }
    m_PoseValid = true;
    return true;
}

// 기존 Update 호출은 물리 override 없이 두 CPU pose 단계를 연속 수행한다.
void ModelInstance::Update(float deltaTime)
{
    // 애니메이션을 진행한 뒤 기존과 같은 node pose와 본 팔레트를 공개한다.
    EvaluateAnimation(deltaTime);
    FinalizePose(nullptr, 0);
}
