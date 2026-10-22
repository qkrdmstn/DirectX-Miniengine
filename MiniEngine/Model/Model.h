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
#include "../Core/GpuBuffer.h"
#include "../Core/VectorMath.h"
#include "../Core/Camera.h"
#include "../Core/CommandContext.h"
#include "../Core/UploadBuffer.h"
#include "../Core/TextureManager.h"
#include "../Core/Math/BoundingBox.h"
#include "../Core/Math/BoundingSphere.h"
#include <cstdint>
#include <memory>
#include <vector>

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

struct GraphNode // 96 bytes
{
    Math::Matrix4 xform;
    Math::Quaternion rotation;
    Math::XMFLOAT3 scale;

    uint32_t matrixIdx : 28;
    uint32_t hasSibling : 1;
    uint32_t hasChildren : 1;
    uint32_t staleMatrix : 1;
    uint32_t skeletonRoot : 1;
};

struct Joint
{
    Math::Matrix4 posXform;
    Math::Matrix3 nrmXform;
};

struct SkinPalette
{
    uint32_t startJoint; // Offset into the flattened joint array
    uint32_t numJoints;  // Local jointIndices are relative to this palette
};

class Model
{
public:

    ~Model() { Destroy(); }

    void Render(Renderer::MeshSorter& sorter,
        const GpuBuffer& meshConstants,
        const Math::AffineTransform sphereTransforms[],
        const Joint* skeleton,
        const std::vector<uint8_t>* meshHidden = nullptr,           // meshHidden[i] != 0 skips the i-th mesh
        const std::vector<std::unique_ptr<ByteAddressBuffer>>* vertexColors = nullptr) const;     // vertexColors[i], if set, overrides the i-th mesh's vertex colors

    Math::BoundingSphere m_BoundingSphere; // Object-space bounding sphere
    Math::AxisAlignedBox m_BoundingBox;
    ByteAddressBuffer m_DataBuffer;
    std::vector<uint8_t> m_GeometryCPU; // CPU copy of m_DataBuffer; kept for every model (mesh cutting reads it)
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
    std::vector<SkinPalette> m_SkinPalettes;
    std::vector<uint32_t> m_NodeParents;
    std::vector<std::string> m_NodeNames;

protected:
    void Destroy();
};

class ModelInstance
{
public:
    ModelInstance() {}
    ~ModelInstance() {
        m_MeshConstantsCPU.Destroy();
        m_MeshConstantsGPU.Destroy();
    }
    ModelInstance( std::shared_ptr<const Model> sourceModel );
    ModelInstance( const ModelInstance& modelInstance );

    ModelInstance& operator=( std::shared_ptr<const Model> sourceModel );

    bool IsNull(void) const { return m_Model == nullptr; }

    void Update(GraphicsContext& gfxContext, float deltaTime);
    void Render(Renderer::MeshSorter& sorter) const;

    // Per-mesh visibility (like Unreal SetVisibility): hidden meshes skip color, depth and shadow passes. Out-of-range indices are ignored.
    void SetMeshVisibility(uint32_t meshIndex, bool visible);
    bool IsMeshVisible(uint32_t meshIndex) const;

    // Per-instance vertex colors for a skinned mesh (like Unreal SetVertexColorOverride): one RGBA8 per vertex, in the mesh's vertex buffer order.
    // R >= 0.5 clips the pixel in color, depth and shadow passes. Returns false (and changes nothing) if the mesh is not skinned or the count differs.
    bool SetVertexColorOverride(uint32_t meshIndex, const std::vector<uint32_t>& colors);
    void ClearVertexColorOverride(uint32_t meshIndex);

    // Debug stand-in for Unreal BreakConstraint: pins the node at its world matrix from the last Update and moves it by offset
    // (an immediate shift instead of Unreal's Impulse). Descendants still animate on top of it. Does nothing if the node is already broken.
    void BreakConstraint(const Math::Vector3& offset, uint32_t nodeIndex);
    void ResetBrokenConstraints();
    bool IsConstraintBroken(uint32_t nodeIndex) const;

    void Resize(float newRadius);
    void SetPosition(const Math::Vector3& position);    // world position of the model origin (scale is kept)
    Math::Vector3 GetCenter() const;
    Math::Scalar GetRadius() const;
    Math::BoundingSphere GetBoundingSphere() const;
    Math::OrientedBox GetBoundingBox() const;

    size_t GetNumAnimations(void) const { return m_AnimState.size(); }
    const Model* GetModel(void) const { return m_Model.get(); }
    // CPU copies of the matrices written by the last Update (indexed by matrixIdx / flat joint index)
    const Math::Matrix4* GetNodeWorldMatrices(void) const { return m_NodeWorld.get(); }
    const Joint* GetSkeleton(void) const { return m_Skeleton.get(); }
    AnimationState::eMode GetAnimationMode(uint32_t animIdx) const
    {
        return animIdx < m_AnimState.size() ? m_AnimState[animIdx].state : AnimationState::kStopped;
    }
    void PlayAnimation(uint32_t animIdx, bool loop);
    void PauseAnimation(uint32_t animIdx);
    void ResetAnimation(uint32_t animIdx);
    void StopAnimation(uint32_t animIdx);
    void UpdateAnimations(float deltaTime);
    void LoopAllAnimations(void);

private:
    std::shared_ptr<const Model> m_Model;
    UploadBuffer m_MeshConstantsCPU;
    ByteAddressBuffer m_MeshConstantsGPU;
    std::unique_ptr<Math::AffineTransform[]> m_BoundingSphereTransforms;
    std::unique_ptr<Math::Matrix4[]> m_NodeWorld;
    Math::UniformTransform m_Locator;

    std::unique_ptr<GraphNode[]> m_AnimGraph;   // A copy of the scene graph when instancing animation
    std::vector<AnimationState> m_AnimState;    // Per-animation (not per-curve)
    std::unique_ptr<Joint[]> m_Skeleton;
    std::vector<uint8_t> m_MeshHidden;          // Hidden flag per mesh index (reset to all visible when the model changes)
    struct BrokenConstraint { uint32_t node; Math::Matrix4 world; };
    std::vector<BrokenConstraint> m_BrokenConstraints;      // Pinned node worlds used by Update instead of parent * local (reset when the model changes)
    std::vector<std::unique_ptr<ByteAddressBuffer>> m_VertexColors;     // Vertex color override per mesh index (null = none; reset when the model changes)
};
