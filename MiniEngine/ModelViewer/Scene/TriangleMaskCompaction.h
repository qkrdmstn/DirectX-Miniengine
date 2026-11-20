#pragma once
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace Viewer
{
    struct MaskDraw
    {
        uint32_t IndexCount, BaseVertex;
    };
    struct MaskSection
    {
        bool Skinned, R32;
        const std::vector<uint32_t>* Indices; // draw 순서로 펼친, baseVertex가 더해진 CPU 인덱스
        std::vector<MaskDraw> Draws;
    };
    struct CompactMaskSection
    {
        bool Override = false;
        uint32_t ByteOffset = 0, ByteCount = 0;
        std::vector<std::pair<uint32_t, uint32_t>> Draws; // StartIndex, IndexCount
    };
    struct CompactMask
    {
        std::vector<uint8_t> Bytes;
        std::vector<CompactMaskSection> Sections;
    };

    // GPU 자원 없이 제거 상태를 원본 형식 IB로 압축한다. 거절 시 기존 출력은 보존한다.
    inline bool CompactTriangleMask(const std::vector<MaskSection>& sections,
        const std::vector<uint32_t>& state, CompactMask& output)
    {
        // 상태 길이와 예약 비트를 확인한 뒤 임시 결과만 변경한다.
        size_t total = 0;
        for (const auto& section : sections)
            for (const auto& draw : section.Draws) total += draw.IndexCount / 3;
        if (!total || total != state.size()) return false;
        for (uint32_t value : state) if (value & 0x7fff0000u) return false;
        CompactMask next;
        size_t triangle = 0;

        // 제거가 있는 section만 override하며 section·draw·삼각형 순서를 유지한다.
        for (const auto& section : sections)
        {
            size_t count = 0;
            for (const auto& draw : section.Draws) count += draw.IndexCount / 3;
            next.Sections.emplace_back();
            auto& view = next.Sections.back();
            view.Override = std::any_of(state.begin() + triangle, state.begin() + triangle + count,
                [](uint32_t value) { return (value & 0x80000000u) != 0; });
            if (!view.Override) { triangle += count; continue; }
            if (!section.Skinned || !section.Indices || section.Indices->size() != count * 3) return false;

            // 혼합 R16/R32 section의 시작을 DWORD에 맞추고 원본 draw 상대값을 저장한다.
            next.Bytes.resize((next.Bytes.size() + 3) & ~size_t(3));
            view.ByteOffset = uint32_t(next.Bytes.size());
            const uint32_t stride = section.R32 ? 4 : 2;
            size_t source = 0;
            for (const auto& draw : section.Draws)
            {
                const uint32_t start = uint32_t(next.Bytes.size() - view.ByteOffset) / stride;
                for (uint32_t t = 0; t < draw.IndexCount / 3; ++t, ++triangle, source += 3)
                {
                    if (state[triangle] & 0x80000000u) continue;
                    for (uint32_t k = 0; k < 3; ++k)
                    {
                        const int64_t value = int64_t((*section.Indices)[source + k]) - int32_t(draw.BaseVertex);
                        if (value < 0 || uint64_t(value) > (section.R32 ? UINT32_MAX : UINT16_MAX)) return false;
                        const uint32_t index = uint32_t(value);
                        const size_t offset = next.Bytes.size();
                        next.Bytes.resize(offset + stride);
                        std::memcpy(next.Bytes.data() + offset, &index, stride);
                    }
                }
                view.Draws.emplace_back(start, uint32_t(next.Bytes.size() - view.ByteOffset) / stride - start);
            }
            view.ByteCount = uint32_t(next.Bytes.size() - view.ByteOffset);
        }

        // 전체 검증이 성공한 경우에만 이전 결과를 교체한다.
        output = std::move(next);
        return true;
    }
}
