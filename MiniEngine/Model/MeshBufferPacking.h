#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace Renderer
{
    // 한 render mesh 안에서 byte 복사 위치와 draw의 원소 위치를 별도로 누적한다.
    struct MeshBufferPacking
    {
        size_t VertexBytes = 0;
        size_t DepthVertexBytes = 0;
        size_t IndexBytes = 0;
        uint32_t VertexCount = 0;
        uint32_t IndexCount = 0;

        struct DrawLocation
        {
            uint32_t BaseVertex;
            uint32_t StartIndex;
        };

        // primitive의 세 stream을 이어 붙이고 해당 draw의 원소 단위 시작 위치를 반환한다.
        DrawLocation Append(
            uint8_t* vertices, uint8_t* depthVertices, uint8_t* indices,
            const void* vertexData, size_t vertexBytes, uint32_t vertexStride,
            const void* depthData, size_t depthBytes,
            const void* indexData, size_t indexBytes, uint32_t indexStride)
        {
            // draw offset은 vertex/index 개수이며 메모리 복사 위치는 byte 단위다.
            assert(vertexStride != 0 && vertexBytes % vertexStride == 0);
            assert((indexStride == 2 || indexStride == 4) && indexBytes % indexStride == 0);
            const DrawLocation location = { VertexCount, IndexCount };

            // 각 stream의 누적 byte 위치에 복사해 이전 primitive를 보존한다.
            std::memcpy(vertices + VertexBytes, vertexData, vertexBytes);
            std::memcpy(depthVertices + DepthVertexBytes, depthData, depthBytes);
            std::memcpy(indices + IndexBytes, indexData, indexBytes);

            // 다음 primitive의 byte 위치와 draw 원소 위치를 각각 갱신한다.
            VertexBytes += vertexBytes;
            DepthVertexBytes += depthBytes;
            IndexBytes += indexBytes;
            VertexCount += static_cast<uint32_t>(vertexBytes / vertexStride);
            IndexCount += static_cast<uint32_t>(indexBytes / indexStride);
            return location;
        }
    };
}
