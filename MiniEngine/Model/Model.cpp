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

// 모델의 원본 GPU 버퍼와 로드 시 만든 격자 버퍼를 해제한다.
void Model::Destroy()
{
    // 격자 버퍼와 셀 수를 함께 비워 명시적 재호출도 허용한다.
    m_GpuLatticeTopology.SectionVertexCell.clear();
    m_GpuLatticeTopology.CellCount = 0;

    // 기존 모델 데이터와 그래프를 초기화한다.
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
    m_AnimNodeTransforms = std::make_unique<Matrix4[]>(m_Model->m_NumNodes);
    m_Skeleton = std::make_unique<Joint[]>(m_Model->m_NumJoints);
    m_AnimSkeleton = std::make_unique<Joint[]>(m_Model->m_NumJoints);
    BuildPoseParents(m_Model->m_SceneGraph.get(), m_Model->m_NumNodes, m_PoseParents);

    // 애니메이션이 있는 모델만 수정 가능한 local 그래프를 복사한다.
    if (m_Model->m_NumAnimations > 0)
    {
        m_AnimGraph = std::make_unique<GraphNode[]>(m_Model->m_NumNodes);
        std::memcpy(m_AnimGraph.get(), m_Model->m_SceneGraph.get(), m_Model->m_NumNodes * sizeof(GraphNode));
        m_AnimState.resize(m_Model->m_NumAnimations);
    }
}

// node 전역 행렬과 IBM으로 중복 node를 포함한 전체 Joint 배열(posXform, nrmXform)을 채운다.
static void BuildJointArray(const Model& model, const Matrix4* nodes, Joint* joints)
{
    // 팔레트 인덱스 순서를 유지하여 여러 skin의 동일 본도 모두 반영한다.
    for (uint32_t i = 0; i < model.m_NumJoints; ++i)
    {
        // 각 팔레트의 IBM과 normal 행렬을 따로 계산한다.
        Joint& joint = joints[i];
        joint.posXform = nodes[model.m_JointIndices[i]] * model.m_JointIBMs[i];
        joint.nrmXform = InverseTranspose(joint.posXform.Get3x3());
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

    // 물리 override가 덮기 전의 순수 애니메이션 node를 보존한다. 정적 pose도 같은 평가기를 거치므로 최종 pose와 같다.
    std::memcpy(m_AnimNodeTransforms.get(), m_ModelSpaceTransforms.get(), m_Model->m_NumNodes * sizeof(Matrix4));
    m_OverrideNodes.clear();

    // 순수 애니메이션 node에서 owner당 한 번 애니메이션 Joint 배열을 만든다. 잘못된 joint 참조는 건너뛴다.
    bool jointsValid = true;
    for (uint32_t i = 0; i < m_Model->m_NumJoints; ++i)
    {
        jointsValid = jointsValid && m_Model->m_JointIndices[i] < m_Model->m_NumNodes;
    }
    if (jointsValid)
        BuildJointArray(*m_Model, m_AnimNodeTransforms.get(), m_AnimSkeleton.get());
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
    BuildJointArray(*m_Model, m_ModelSpaceTransforms.get(), m_Skeleton.get());

    // 진단이 override 노드와 자손만 달라야 하는 조건을 검사하도록 이번 override 목록을 기록한다.
    m_OverrideNodes.clear();
    for (uint32_t i = 0; i < overrideCount; ++i)
        m_OverrideNodes.push_back(overrides[i].Node);
    m_PoseValid = true;
    return true;
}

// 지정한 클립의 현재 주기 내 재생 시간을 반환한다.
float ModelInstance::GetAnimationTime(uint32_t animIdx) const
{
    // 잘못된 클립 번호는 다른 조회 API처럼 안전한 기본값을 반환한다.
    return animIdx < m_AnimState.size() ? m_AnimState[animIdx].time : 0.0f;
}

// 기존 Update 호출은 물리 override 없이 두 CPU pose 단계를 연속 수행한다.
void ModelInstance::Update(float deltaTime)
{
    // 애니메이션을 진행한 뒤 기존과 같은 node pose와 본 팔레트를 공개한다.
    EvaluateAnimation(deltaTime);
    FinalizePose(nullptr, 0);
}
