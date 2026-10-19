#pragma once

#include "ProceduralMesh.h"
#include "Geometry/PatchWeld.h"
#include "VectorMath.h"
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

    // Source triangle of one patch triangle, parallel to ProceduralMeshData::indices / 3.
    // Identifies the original triangle to hide or merge back later.
    struct BonePatchTriangle
    {
        uint32_t meshSlot;          // Index into BonePatchSource::meshes
        uint32_t drawIndex;         // Index into Mesh::draw[]
        uint32_t primitive;         // Triangle number within that draw
    };

    struct BonePatchSource
    {
        uint32_t paletteId = 0;
        uint32_t jointIndex = 0;
        float minWeight = 0.0f;
        std::vector<BonePatchMesh> meshes;
        std::vector<BonePatchVertex> vertices;
        std::vector<BonePatchTriangle> triangles;
        // World matrices of every palette joint at extraction time, in the same space as the
        // patch vertices (mesh world * joint node world). Slice planes and caps attach to these.
        std::vector<Math::Matrix4> poseJointWorld;
        // Triangles left outside the patch that are weighted both to the selected joint's
        // subtree and to other joints. They would stretch once the subtree is separated, so a
        // non-zero count means the selected joint is not a clean cut candidate.
        uint32_t mixedOutsideTriangles = 0;
    };

    // Copies every triangle with at least one vertex whose weight for the palette-local joint
    // exceeds minWeight. Only the selected joint counts: triangles weighted to its descendants
    // alone stay on the original mesh and follow those bones after separation.
    // Patch vertices are posed with the instance matrices of its last Update and stored in
    // world space, so the patch must be drawn with an identity (or offset) world.
    // On failure the outputs are left unchanged.
    bool ExtractBonePatch(const Model& model, const ModelInstance& instance,
        uint32_t paletteId, uint32_t jointIndex, float minWeight,
        ProceduralMeshData& outMesh, BonePatchSource& outSource, std::string* error = nullptr);

    // Flattens an extraction result into the plain input of Geometry/BuildPatchPiece, so the
    // slicing code never sees Model or Core types. The two inputs must come from the same
    // ExtractBonePatch call.
    bool MakeWeldInput(const ProceduralMeshData& mesh, const BonePatchSource& source,
        WeldInput& out, std::string* error = nullptr);
}
