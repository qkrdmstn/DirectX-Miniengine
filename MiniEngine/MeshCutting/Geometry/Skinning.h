#pragma once

// 스킨 정점 표와 스키닝 계산(바인드 포즈 -> 현재 포즈), joint 집합 판정. Model, Core에 의존하지 않는다 (설계: Docs/ClassLayoutDesign.md 1.2절).

#include "Patch.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace MeshCutting
{
    // 용접 뒤 원본 정점 하나의 스킨 (joints[0..jointCount)는 오름차순이고 weights > 0)
    struct SkinVertex
    {
        uint32_t meshSlot = 0;
        DirectX::XMFLOAT3 bindPosition = { 0.0f, 0.0f, 0.0f };
        uint32_t jointCount = 0;
        uint16_t joints[4] = {};
        float weights[4] = {};
    };

    struct SkinStats
    {
        uint32_t skinVerticesEvaluated = 0;     // 계산한 선형 혼합 수
        uint32_t surfaceVerticesWritten = 0;
        uint32_t capVerticesSkipped = 0;        // Cap 정점은 위치를 유지한다
    };

    // 한 패치에서 잘려 나온 piece들이 shared_ptr<const Skinning>으로 공유한다. 포즈 행렬은 Apply의 인자로 받는다.
    class Skinning
    {
    public:
        explicit Skinning(std::vector<SkinVertex> vertices) : m_Vertices(std::move(vertices)) {}

        const std::vector<SkinVertex>& GetVertices() const { return m_Vertices; }

        // slotMatrices[meshSlot][joint]로 piece의 모든 정점을 현재 포즈로 옮긴다. 실패하면 piece를 바꾸지 않는다.
        bool Apply(PatchPiece& piece, const std::vector<std::vector<DirectX::XMFLOAT4X4>>& slotMatrices,
            SkinStats* stats = nullptr, std::string* error = nullptr) const;

        // 바인드 포즈 정점 하나를 joint 가중치의 선형 합으로 현재 월드 위치로 옮긴다.
        static DirectX::XMVECTOR SkinPoint(const DirectX::XMFLOAT4X4* matrices, uint32_t matrixCount,
            const uint16_t* joints, const float* weights, uint32_t entryCount, const DirectX::XMFLOAT3& bindPosition);

        // 정점의 4개 슬롯 중 joint에 대한 가중치의 합
        static float Influence(const uint16_t joints[4], const float weights[4], uint32_t joint);

        // 집합 안 joint들의 Influence 합
        static float SetInfluence(const uint16_t joints[4], const float weights[4], const std::vector<uint32_t>& jointSet);

        // 집합 안 joint 중 하나라도 Influence가 minWeight를 넘으면 참
        static bool InJointSet(const uint16_t joints[4], const float weights[4], const std::vector<uint32_t>& jointSet,
            float minWeight);

        // 오름차순, 중복 없음, 모든 값이 jointCount 미만이면 참 (비어 있으면 거짓)
        static bool IsValidJointSet(const std::vector<uint32_t>& jointSet, uint32_t jointCount);

    private:
        std::vector<SkinVertex> m_Vertices;
    };
}
