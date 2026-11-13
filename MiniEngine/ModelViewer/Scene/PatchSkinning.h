#pragma once
#include "Model.h"
#include <vector>

class GeometryRevision;
struct GeometryBuffers;

namespace Viewer
{
    struct PatchSkinVertices
    {
        ByteAddressBuffer Data;
        uint32_t Flags = 0, Stride = 0;
    };

    struct PatchSkinSnapshot
    {
        uint32_t AttachNode = UINT32_MAX;
        uint32_t MeshNode = UINT32_MAX; // 공통 N을 제공한 실제 추출 section의 meshCBV다.
        Math::Matrix4 AttachAtCut{ Math::kIdentity };
        Math::Matrix4 MeshNodeAtCut{ Math::kIdentity };
        std::vector<Joint> JointsAtCut;
        std::vector<uint8_t> Frozen;
        bool SkinningAllowed = true;
    };

    // 렌더 연결 전까지 조각의 skin 입력과 section별 유효 정점 범위를 함께 보관한다.
    struct PatchSkinPublication
    {
        struct Section
        {
            uint32_t FirstVertex, VertexCount;
        };
        PatchSkinSnapshot Snapshot;
        PatchSkinVertices Vertices;
        std::vector<Section> Sections;
        uint32_t VertexCount = 0;
    };

    // 현재 팔레트의 고정 본을 부착 노드 상대 pose로 바꾸고 강체 슬롯 K를 덧붙인다.
    void BuildPatchPalette(const PatchSkinSnapshot& snapshot, const Joint* currentJoints,
        const Math::Matrix4* currentNodes, std::vector<Joint>& out);

    // 원본 revision과 추출 출처를 이용해 조각의 skin VB를 만들고 GPU 완료 후 반환한다.
    bool BuildPatchSkinVertices(const GeometryRevision& source, ByteAddressBuffer& triangleSources,
        const GeometryBuffers& piece, const PatchSkinSnapshot& snapshot, PatchSkinVertices& out);
}
