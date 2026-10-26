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

#pragma once

#include "Animation.h"
#include "ModelPose.h"
#include "PosedGeometry.h"
#include "TransformBounds.h"
#include "../Core/GpuBuffer.h"
#include "../Core/VectorMath.h"
#include "../Core/Camera.h"
#include "../Core/CommandContext.h"
#include "../Core/UploadBuffer.h"
#include "../Core/TextureManager.h"
#include "../Core/Math/BoundingBox.h"
#include "../Core/Math/BoundingSphere.h"
#include <cstdint>
#include <vector>
// Model 모듈이 Jolt 없이 충돌 hull을 만들도록 순수 C++ 충돌 소스(ConvexElem/AggregateGeom)를 Model.vcxproj에 직접 컴파일한다.
// MiniPhysics.lib에는 이 객체가 없으므로 중복 심볼이 생기지 않는다.
#include "../Physics/Collision/AggregateGeom.h"

namespace Renderer
{
    class MeshSorter;
}

//
// To request a PSO index, provide flags that describe the kind of PSO
// you need.  If one has not yet been created, it will be created.
//
namespace PSOFlags
{
    enum : uint16_t
    { 
        kHasPosition    = 0x001,  // Required
        kHasNormal      = 0x002,  // Required
        kHasTangent     = 0x004,
        kHasUV0         = 0x008,  // Required (for now)
        kHasUV1         = 0x010,
        kAlphaBlend     = 0x020,
        kAlphaTest      = 0x040,
        kTwoSided       = 0x080,
        kHasSkin        = 0x100,  // Implies having indices and weights
    };
}

struct Mesh
{
    float    bounds[4];     // A bounding sphere
    uint32_t vbOffset;      // BufferLocation - Buffer.GpuVirtualAddress
    uint32_t vbSize;        // SizeInBytes
    uint32_t vbDepthOffset; // BufferLocation - Buffer.GpuVirtualAddress
    uint32_t vbDepthSize;   // SizeInBytes
    uint32_t ibOffset;      // BufferLocation - Buffer.GpuVirtualAddress
    uint32_t ibSize;        // SizeInBytes
    uint8_t  vbStride;      // StrideInBytes
    uint8_t  ibFormat;      // DXGI_FORMAT
    uint16_t meshCBV;       // Index of mesh constant buffer
    uint16_t materialCBV;   // Index of material constant buffer
    uint16_t srvTable;      // Offset into SRV descriptor heap for textures
    uint16_t samplerTable;  // Offset into sampler descriptor heap for samplers
    uint16_t psoFlags;      // Flags needed to request a PSO
    uint16_t pso;           // Index of pipeline state object
    uint16_t numJoints;     // Number of skeleton joints when skinning
    uint16_t startJoint;    // Flat offset to first joint index
    uint16_t numDraws;      // Number of draw groups

    struct Draw
    {
        uint32_t primCount;   // Number of indices = 3 * number of triangles
        uint32_t startIndex;  // Offset to first index in index buffer 
        uint32_t baseVertex;  // Offset to first vertex in vertex buffer
    };
    Draw draw[1];           // Actually 1 or more draws
};

// 메시별 임포트 시점 충돌 생성 결과 상태다.
enum class MeshCollisionStatus : uint8_t
{
    Generated,      // hull 생성 성공
    Skinned,        // 스킨 메시라 충돌 없음
    NodeAnimated,   // 메시 노드나 조상 노드가 애니메이션 대상이라 충돌 없음
    DecodeFailed,   // 정점 위치를 해석할 수 없음
    GenerateFailed  // 18-DOP hull 생성 실패
};

struct Joint
{
    Math::Matrix4 posXform;
    Math::Matrix3 nrmXform;
};

class Model
{
public:

    ~Model() { Destroy(); }

    Math::BoundingSphere m_BoundingSphere; // Object-space bounding sphere
    Math::AxisAlignedBox m_BoundingBox;
    ByteAddressBuffer m_DataBuffer;
    ByteAddressBuffer m_MaterialConstants;
    uint32_t m_NumNodes;
    uint32_t m_NumMeshes;
    uint32_t m_NumAnimations;
    uint32_t m_NumJoints;
    std::unique_ptr<uint8_t[]> m_MeshData;
    std::unique_ptr<GraphNode[]> m_SceneGraph;
    std::vector<TextureRef> textures;
    std::unique_ptr<uint8_t[]> m_KeyFrameData;
    std::unique_ptr<AnimationCurve[]> m_CurveData;
    std::unique_ptr<AnimationSet[]> m_Animations;
    std::unique_ptr<uint16_t[]> m_JointIndices;
    std::unique_ptr<Math::Matrix4[]> m_JointIBMs;
    std::vector<MiniCollision::AggregateGeom> m_MeshCollision;   // m_MeshData 순서의 model-local 충돌(생성되지 않은 메시는 비어 있음)
    std::vector<MeshCollisionStatus> m_MeshCollisionStatus;      // m_MeshData 순서의 충돌 생성 상태

    // 메시 하나의 model-local 충돌 형상을 읽는다.
    const MiniCollision::AggregateGeom& GetMeshCollision(uint32_t meshIndex) const
    {
        // 메시 순서의 충돌 형상을 돌려준다.
        return m_MeshCollision[meshIndex];
    }
    // 메시 하나의 충돌 생성 상태를 읽는다.
    MeshCollisionStatus GetMeshCollisionStatus(uint32_t meshIndex) const
    {
        // 메시 순서의 충돌 생성 상태를 돌려준다.
        return m_MeshCollisionStatus[meshIndex];
    }

protected:
    void Destroy();
};

class ModelInstance
{
public:
    // 원본과 애니메이션 pose만 소유하며 GPU 렌더 자원은 MeshComponent가 소유한다.
    explicit ModelInstance(std::shared_ptr<const Model> sourceModel);
    // 애니메이션 상태의 암묵적인 복사·대입을 금지한다.
    ModelInstance(const ModelInstance&) = delete;
    // 원본 pose 소유자의 복사 대입을 금지한다.
    ModelInstance& operator=(const ModelInstance&) = delete;
    // 공유 원본의 소유권을 읽는다.
    const std::shared_ptr<const Model>& GetModel() const { return m_Model; }
    // 이 프레임에 평가된 node pose를 읽는다.
    const Math::Matrix4* GetNodeTransforms() const { return m_ModelSpaceTransforms.get(); }
    // 컴포넌트들이 읽기 전용으로 공유하는 본 팔레트를 읽는다.
    const Joint* GetJoints() const { return m_Skeleton.get(); }
    // 잘못된 node 그래프의 이전 pose를 렌더에 사용하지 않도록 검사한다.
    bool HasValidPose() const { return m_PoseValid; }
    // GameObject당 한 번 CPU 애니메이션·node·본 pose를 평가한다.
    void Update(float deltaTime);

    size_t GetNumAnimations(void) const { return m_AnimState.size(); }
    void PlayAnimation(uint32_t animIdx, bool loop);
    void PauseAnimation(uint32_t animIdx);
    void ResetAnimation(uint32_t animIdx);
    void StopAnimation(uint32_t animIdx);
    void UpdateAnimations(float deltaTime);
    void LoopAllAnimations(void);

private:
    std::shared_ptr<const Model> m_Model;
    std::unique_ptr<Math::Matrix4[]> m_ModelSpaceTransforms;
    bool m_PoseValid = false;

    std::unique_ptr<GraphNode[]> m_AnimGraph;   // A copy of the scene graph when instancing animation
    std::vector<AnimationState> m_AnimState;    // Per-animation (not per-curve)
    std::unique_ptr<Joint[]> m_Skeleton;
};
