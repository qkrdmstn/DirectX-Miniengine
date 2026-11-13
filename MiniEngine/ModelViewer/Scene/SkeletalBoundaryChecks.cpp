#include "SkeletalBoundaryChecks.h"
#include "ReadbackBuffer.h"
#include "CommandContext.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace Viewer
{
    namespace
    {
        // GPU 버퍼를 읽고 지정한 정점 stride의 float3 위치를 복원한다.
        std::vector<Math::Vector3> ReadPositions(const ByteAddressBuffer& source, uint32_t stride,
            D3D12_RESOURCE_STATES finalState)
        {
            // 같은 크기의 readback 버퍼로 복사하고 GPU 완료를 기다린다.
            const uint32_t size = uint32_t(source.GetBufferSize());
            if (size < stride)
            {
                return {};
            }
            ReadbackBuffer readback;
            readback.Create(L"Boundary check readback", size, 1);
            auto& context = GraphicsContext::Begin(L"Boundary check copy");
            auto& buffer = const_cast<ByteAddressBuffer&>(source);
            context.CopyBuffer(readback, buffer);
            context.TransitionResource(buffer, finalState);
            context.Finish(true);

            // 매핑한 내용을 복사해 readback 수명과 분리한다.
            const auto* bytes = static_cast<const uint8_t*>(readback.Map());
            if (!bytes)
            {
                return {};
            }
            std::vector<Math::Vector3> positions(size / stride);
            for (size_t i = 0; i < positions.size(); ++i)
            {
                float p[3];
                std::memcpy(p, bytes + i * stride, sizeof(p));
                positions[i] = Math::Vector3(p[0], p[1], p[2]);
            }
            readback.Unmap();
            return positions;
        }

    }

    // Data 위치로 짝을 찾고 원본과 조각의 section·정점 식별자를 보존한다.
    bool PairBoundary(const MeshComponent* const pieces[2], const std::vector<BoundaryVertex>& boundary,
        std::vector<BoundaryPair>& pairs)
    {
        // 모든 원본 경계 정점에 대응 정점이 존재하는지도 검사한다.
        std::vector<bool> matched(boundary.size(), false);

        // 조각의 모든 mesh 정점 중 경계 정점과 같은 위치만 짝으로 기록한다.
        pairs.clear();
        for (uint32_t piece = 0; piece < 2; ++piece)
        {
            if (!pieces[piece] || !pieces[piece]->GetGeometry() || !pieces[piece]->GetGeometry()->GetGenerated())
            {
                return false;
            }
            const auto& geometry = *pieces[piece]->GetGeometry();
            const auto& baked = *geometry.GetGenerated();
            const auto positions = ReadPositions(baked.Data, 28, D3D12_RESOURCE_STATE_GENERIC_READ);
            for (size_t s = 0; s < geometry.GetSections().size(); ++s)
            {
                const auto& mesh = geometry.GetMesh(s);
                if (mesh.materialCBV == 0xffff)
                {
                    continue;
                }
                const size_t base = mesh.vbOffset / 28;
                for (uint32_t v = 0; v < mesh.vbSize / mesh.vbStride; ++v)
                {
                    if (base + v >= positions.size())
                    {
                        return false;
                    }
                    const auto position = positions[base + v];
                    for (uint32_t i = 0; i < boundary.size(); ++i)
                        if (float(Math::Length(position - boundary[i].Local)) <= 1e-5f)
                        {
                            pairs.push_back({ piece, i, uint32_t(s), v, boundary[i].Section, boundary[i].Index });
                            matched[i] = true;
                        }
                }
            }
        }

        // 원본 경계 정점의 누락 여부를 집계한다.
        Utility::Printf("SP7e2 boundary pairing boundary=%zu matched=%zu pairs=%zu\n", boundary.size(),
            std::count(matched.begin(), matched.end(), true), pairs.size());
        return std::all_of(matched.begin(), matched.end(), [](bool value) { return value; });
    }

    // 같은 프레임의 원본 PG와 조각 PG 또는 강체 Data로 경계 거리를 측정한다.
    bool MeasureBoundary(const GameObject& owner, const MeshComponent& source, const MeshComponent* const pieces[2],
        const std::vector<BoundaryVertex>& boundary, const std::vector<BoundaryPair>& pairs,
        float& maxError)
    {
        // 측정 실패 시 유효한 거리로 오인하지 않도록 결과를 초기화한다.
        maxError = std::numeric_limits<float>::quiet_NaN();
        if (!source.GetGeometry() || boundary.empty() || pairs.size() < boundary.size())
        {
            return false;
        }

        // 중복 짝이 누락을 가리지 않도록 모든 경계 정점의 대응 여부를 확인한다.
        std::vector<bool> matched(boundary.size(), false);
        for (const auto& pair : pairs)
        {
            if (pair.Boundary >= boundary.size() || pair.Piece >= 2)
            {
                return false;
            }
            if (pair.OriginalSection != boundary[pair.Boundary].Section
                || pair.OriginalVertex != boundary[pair.Boundary].Index)
            {
                return false;
            }
            matched[pair.Boundary] = true;
        }
        if (!std::all_of(matched.begin(), matched.end(), [](bool value) { return value; }))
        {
            return false;
        }

        // 같은 프레임의 원본 pose 전체를 읽고 각 mesh의 시작 위치를 구한다.
        const auto& posed = source.GetPosedGeometry();
        if (!posed.IsReady())
        {
            return false;
        }
        const auto positions = ReadPositions(posed.GetBuffer(), 40, D3D12_RESOURCE_STATE_GENERIC_READ);
        const auto& geometry = *source.GetGeometry();
        const Math::Matrix4 sourceWorld(source.GetWorldTransform(owner.GetTransform()));

        // 활성 skin은 현재 RAW40 PG를 읽고 강체 fallback은 현재 28 B Data를 읽는다.
        std::vector<Math::Vector3> piecePositions[2];
        Math::Matrix4 pieceWorld[2];
        for (uint32_t side = 0; side < 2; ++side)
        {
            const auto* piece = pieces[side];
            if (!piece || !piece->GetGeometry() || !piece->GetGeometry()->GetGenerated())
            {
                return false;
            }
            pieceWorld[side] = Math::Matrix4(piece->GetWorldTransform(owner.GetTransform()));
            if (piece->HasSkinnedPatch())
            {
                if (!piece->GetPosedGeometry().IsReady())
                {
                    return false;
                }
                piecePositions[side] = ReadPositions(piece->GetPosedGeometry().GetBuffer(), 40,
                    D3D12_RESOURCE_STATE_GENERIC_READ);
            }
            else
            {
                piecePositions[side] = ReadPositions(piece->GetGeometry()->GetGenerated()->Data, 28,
                    D3D12_RESOURCE_STATE_GENERIC_READ);
            }
        }

        // section 안의 정점 번호를 버퍼 offset으로 바꾸고 각 컴포넌트의 렌더 월드를 적용한다.
        float maximum = 0;
        for (const auto& pair : pairs)
        {
            const auto& piece = *pieces[pair.Piece];
            const auto& pieceGeometry = *piece.GetGeometry();
            if (pair.OriginalSection >= geometry.GetSections().size()
                || pair.PieceSection >= pieceGeometry.GetSections().size())
            {
                return false;
            }
            const auto& originalMesh = geometry.GetMesh(pair.OriginalSection);
            const auto& pieceMesh = pieceGeometry.GetMesh(pair.PieceSection);
            if (pair.OriginalVertex >= originalMesh.vbSize / originalMesh.vbStride
                || pair.PieceVertex >= pieceMesh.vbSize / pieceMesh.vbStride)
            {
                return false;
            }
            const size_t originalIndex = posed.GetOffset(geometry.GetSections()[pair.OriginalSection].SourceMesh) / 40
                + size_t(pair.OriginalVertex);
            const size_t pieceIndex = (piece.HasSkinnedPatch()
                ? piece.GetPosedGeometry().GetOffset(pieceGeometry.GetSections()[pair.PieceSection].SourceMesh) / 40
                : pieceMesh.vbOffset / 28) + size_t(pair.PieceVertex);
            if (originalIndex >= positions.size() || pieceIndex >= piecePositions[pair.Piece].size())
            {
                return false;
            }
            const Math::Vector3 original(sourceWorld * Math::Vector4(positions[originalIndex], 1));
            const Math::Vector3 current(pieceWorld[pair.Piece] * Math::Vector4(piecePositions[pair.Piece][pieceIndex], 1));
            const float distance = float(Math::Length(original - current));
            if (!std::isfinite(distance))
            {
                return false;
            }
            maximum = std::max(maximum, distance);
        }

        // 모든 짝의 거리가 유효할 때만 최댓값을 측정 결과로 전달한다.
        maxError = maximum;
        return true;
    }
}
