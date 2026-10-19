#pragma once

#include "ProceduralMesh.h"
#include <cstdint>
#include <string>
#include <vector>

class Model;
class ModelInstance;

namespace MeshCutting
{
    // Source mesh of a patch, as laid out in the model's mesh list.
    struct BonePatchMesh
    {
        uint32_t meshIndex;         // Position in Model::m_MeshData
        uint32_t worldMatrixIndex;  // Mesh::meshCBV, indexes ModelInstance::GetNodeWorldMatrices()
        uint32_t startJoint;        // Flat offset of the mesh's skin palette
    };

    // Skin data of one patch vertex, parallel to ProceduralMeshData::vertices.
    // Kept so the patch can later be skinned exactly like its source vertex.
    struct BonePatchVertex
    {
        uint32_t meshSlot;          // Index into BonePatchSource::meshes
        uint32_t sourceVertex;      // Vertex index in the source mesh vertex buffer
        DirectX::XMFLOAT3 position; // Pre-skinning position from the source vertex buffer
        DirectX::XMFLOAT3 normal;
        uint16_t joints[4];         // Palette-local joint indices
        float weights[4];           // Normalized the same way DefaultVS does
    };

    struct BonePatchSource
    {
        uint32_t paletteId = 0;
        uint32_t jointIndex = 0;
        float minWeight = 0.0f;
        std::vector<BonePatchMesh> meshes;
        std::vector<BonePatchVertex> vertices;
    };

    // Copies every triangle with at least one vertex whose weight for the palette-local joint
    // exceeds minWeight. Patch vertices are posed with the instance matrices of its last Update
    // and stored in world space, so the patch must be drawn with an identity (or offset) world.
    // On failure the outputs are left unchanged.
    bool ExtractBonePatch(const Model& model, const ModelInstance& instance,
        uint32_t paletteId, uint32_t jointIndex, float minWeight,
        ProceduralMeshData& outMesh, BonePatchSource& outSource, std::string* error = nullptr);
}
