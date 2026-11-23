#pragma once
#include "Model.h"
#include <vector>
#include <array>

class GeometryRevision;
struct GeometryBuffers;

namespace Viewer
{
    // 확장 skin 정점(Flags 0x200)의 배치: 정점 끝(Stride)에서 거꾸로 센 바이트 거리다.
    //   -96 내부 교점 표시(uint) | -92 보간 t(float)
    //   -88 끝점0 36B | -52 끝점1 36B | -16 기존 joints+weights
    //   끝점 36B = 위치 12 + 법선 4 | 탄젠트 4(+16) | joints 8(+20) | weights 8(+28)
    constexpr uint32_t kSkinExtFlag = 0x200;
    constexpr uint32_t kSkinExtSize = 80;
    constexpr uint32_t kSkinExtInternal = 96;
    constexpr uint32_t kSkinExtT = 92;
    constexpr uint32_t kSkinExtEndpoint0 = 88;
    constexpr uint32_t kSkinEndpointSize = 36;
    constexpr uint32_t kSkinEndpointJoints = 20;
    constexpr uint32_t kSkinEndpointWeights = 28;

    struct PatchSkinVertices
    {
        // Flags kSkinExtFlag: 말미 skin 앞 kSkinExtSize 바이트는 내부 표시/t와 원본 끝점 두 개다.
        ByteAddressBuffer Data;
        uint32_t Flags = 0, Stride = 0;
        std::vector<Math::BoundingSphere> BindBounds;
    };

    struct PatchSkinSnapshot
    {
        uint32_t AttachNode = UINT32_MAX;
        uint32_t MeshNode = UINT32_MAX; // 공통 N을 제공한 실제 추출 section의 meshCBV다.
        uint64_t SourceRevision = 0;
        Math::Matrix4 SourceFrame{ Math::kIdentity };
        bool SourceFrozen = false;
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
        struct RimCapPair
        {
            uint32_t Rim, Cap; // Data 정점 번호이며 누락 cap은 UINT32_MAX다.
            float DistanceAtCut; // t_cut Data에서 rim과 가장 가까운 cap 사이 거리(m).
        };
        std::vector<RimCapPair> RimCapPairs;
        PatchSkinSnapshot Snapshot;
        PatchSkinVertices Vertices;
        std::vector<Section> Sections;
        uint32_t VertexCount = 0;
        const char* Side = "D5";
    };

    // RS-2b 진단 전용: 요청 시점 N과 팔레트 전체의 posXform이다.
    struct RecutCheckInput
    {
        Math::Matrix4 Node{ Math::kIdentity };
        std::vector<Math::Matrix4> Palette;
    };

    // 재절단 skin 생성의 정점 구성과 readback 비용이다.
    struct RecutSkinStats
    {
        uint32_t Vertices = 0, Copied = 0, K = 0, BadK = 0, BadParent = 0;
        double ReadbackMs = 0;
        uint32_t CopyMismatch = 0;                 // RS-2b: 부모 레코드와 바이트가 다른 복사 정점 수.
        float KPositionMax = 0, KNormalMaxDeg = 0; // RS-2b: K 정점 위치 최대 오차(component-local)와 법선 각도 최대(도).
        uint32_t Skipped = 0;                      // RS-2b: 내부 교점(internal=1)이라 독립 스키닝에서 뺀 복사 정점 수.
        float CopiedSkinMax = 0, KFrameDiff = 0;   // RS-2b: 복사 정점 독립 스키닝 최대 오차(local)와 kFrame 대 N·palette[K] 최대 성분 차이.
        bool Checked = false;                      // RS-2b: 진단 검증을 실제로 수행했는지.
    };

    // 부모 skin과 재절단 결과로 자식 skin publication을 만든다(PatchSkinRecutCS). 완료까지 기다려 부모·결과 buffer를 잡지 않는다.
    bool BuildRecutPatchSkinVertices(const PatchSkinPublication& parent, const GeometryBuffers& cutResult,
        const Math::Matrix4& kFrame, PatchSkinPublication& out, RecutSkinStats* stats = nullptr, const RecutCheckInput* check = nullptr);

    // 현재 팔레트의 고정 본을 부착 노드 상대 pose로 바꾸고 강체 슬롯 K를 덧붙인다.
    void BuildPatchPalette(const PatchSkinSnapshot& snapshot, const Joint* currentJoints,
        const Math::Matrix4* currentNodes, std::vector<Joint>& out);

    // 원본 revision과 추출 출처를 이용해 조각의 skin VB를 만들고 GPU 완료 후 반환한다.
    bool BuildPatchSkinVertices(const GeometryRevision& source, ByteAddressBuffer& triangleSources,
        const GeometryBuffers& piece, const PatchSkinSnapshot& snapshot, PatchSkinVertices& out,
        const std::vector<std::array<uint32_t, 2>>& triangleBodies);
}
