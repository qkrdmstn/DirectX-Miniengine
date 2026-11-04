#include "SkeletalBoundaryChecks.h"
#include "ReadbackBuffer.h"
#include "CommandContext.h"
#include <algorithm>
#include <cstring>

namespace Viewer
{
    namespace
    {
        // GPU buffer 전체를 완료까지 기다려 CPU 바이트로 읽는다.
        std::vector<Math::Vector3> ReadPose(const ByteAddressBuffer& source)
        {
            // 같은 크기의 readback 버퍼로 복사하고 GPU 완료를 기다린다.
            const uint32_t size = uint32_t(source.GetBufferSize());
            ReadbackBuffer readback;
            readback.Create(L"Boundary check readback", size, 1);
            auto& context = GraphicsContext::Begin(L"Boundary check copy");
            auto& buffer = const_cast<ByteAddressBuffer&>(source);
            context.CopyBuffer(readback, buffer);
            context.TransitionResource(buffer, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            context.Finish(true);

            // 매핑한 내용을 복사해 readback 수명과 분리한다.
            const auto* bytes = static_cast<const uint8_t*>(readback.Map());
            std::vector<Math::Vector3> positions(size / 40);
            for (size_t i = 0; i < positions.size(); ++i)
            {
                float p[3];
                std::memcpy(p, bytes + i * 40, sizeof(p));
                positions[i] = Math::Vector3(p[0], p[1], p[2]);
            }
            readback.Unmap();
            return positions;
        }

    }

    // 두 조각의 baked pose를 읽어 C 시점 경계 정점과 같은 위치끼리 짝짓는다.
    bool PairBoundary(const MeshComponent* const pieces[2], const std::vector<BoundaryVertex>& boundary,
        std::vector<BoundaryPair>& pairs)
    {
        // 모든 원본 경계 정점에 대응 정점이 존재하는지도 검사한다.
        std::vector<bool> matched(boundary.size(), false);

        // 조각의 모든 mesh 정점 중 경계 정점과 같은 위치만 짝으로 기록한다.
        pairs.clear();
        for (uint32_t piece = 0; piece < 2; ++piece)
        {
            const auto& geometry = *pieces[piece]->GetGeometry();
            const auto& baked = *geometry.GetGenerated();
            const auto positions = ReadPose(baked.Pose);
            for (size_t s = 0; s < geometry.GetSections().size(); ++s)
            {
                const auto& mesh = geometry.GetMesh(s);
                if (mesh.materialCBV == 0xffff) continue;
                const size_t base = baked.PoseOffsets.at(geometry.GetSections()[s].SourceMesh);
                for (uint32_t v = 0; v < mesh.vbSize / mesh.vbStride; ++v)
                {
                    const auto position = positions.at(base / 40 + v);
                    for (uint32_t i = 0; i < boundary.size(); ++i)
                        if (float(Math::Length(position - boundary[i].Local)) <= 1e-5f)
                        {
                            pairs.push_back({ piece, i, position });
                            matched[i] = true;
                        }
                }
            }
        }
        Utility::Printf("SP7e2 boundary pairing boundary=%zu matched=%zu pairs=%zu\n", boundary.size(),
            std::count(matched.begin(), matched.end(), true), pairs.size());
        return std::all_of(matched.begin(), matched.end(), [](bool value) { return value; });
    }

    // 현재 프레임의 원본 pose와 부착 A/B의 외곽 경계 최대 거리를 잰다.
    bool MeasureBoundary(const GameObject& owner, const MeshComponent& source, const MeshComponent* const pieces[2],
        const std::vector<BoundaryVertex>& boundary, const std::vector<BoundaryPair>& pairs,
        float& maxError)
    {
        // 같은 프레임의 원본 pose 전체를 읽고 각 mesh의 시작 위치를 구한다.
        const auto& posed = source.GetPosedGeometry();
        if (!posed.IsReady()) return false;
        const auto positions = ReadPose(posed.GetBuffer());
        const auto& geometry = *source.GetGeometry();
        const Math::Matrix4 sourceWorld(source.GetWorldTransform(owner.GetTransform()));

        // 조각별 월드 변환을 현재 부착 결과에서 읽는다.
        const Math::Matrix4 pieceWorld[2] = { Math::Matrix4(pieces[0]->GetWorldTransform(owner.GetTransform())),
            Math::Matrix4(pieces[1]->GetWorldTransform(owner.GetTransform())) };
        maxError = 0;

        // 짝마다 조각 정점과 원본 경계 정점의 월드 거리를 비교한다.
        for (const auto& pair : pairs)
        {
            const auto& vertex = boundary[pair.Boundary];
            const size_t offset = size_t(posed.GetOffset(geometry.GetSections()[vertex.Section].SourceMesh))
                + size_t(vertex.Index) * 40;
            const Math::Vector3 original(sourceWorld * Math::Vector4(positions.at(offset / 40), 1));
            const Math::Vector3 piece(pieceWorld[pair.Piece] * Math::Vector4(pair.Local, 1));
            maxError = std::max(maxError, float(Math::Length(original - piece)));
        }
        return true;
    }
}
