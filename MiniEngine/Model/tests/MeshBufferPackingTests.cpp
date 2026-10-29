// Developer PowerShell, repository root (build output stays outside the repository):
// cl /nologo /EHsc /W4 /std:c++17 MiniEngine/Model/tests/MeshBufferPackingTests.cpp /Fo"$env:TEMP/MeshBufferPackingTests.obj" /Fe"$env:TEMP/MeshBufferPackingTests.exe"
// & "$env:TEMP/MeshBufferPackingTests.exe"

#include "../MeshBufferPacking.h"
#include <array>
#include <cstdio>
#include <stdexcept>

// Release에서도 검사 실패를 예외로 전달한다.
static void Require(bool condition, const char* message)
{
    // 조건이 거짓이면 검사 원인을 보고하도록 호출자에게 전달한다.
    if (!condition)
        throw std::runtime_error(message);
}

// 서로 다른 세 primitive를 실제 converter helper로 합쳐 stream과 draw 위치를 검사한다.
template<typename Index>
static void CheckPacking()
{
    // 예상 stream은 입력을 순서대로 연결한 고정 byte와 index 목록이다.
    const uint8_t vertices[] = {
        1, 2, 3, 4, 5, 6, 7, 8,
        9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20,
        21, 22, 23, 24
    };
    const uint8_t depthVertices[] = {
        31, 32, 33, 34, 35, 36,
        37, 38, 39, 40, 41, 42, 43, 44, 45,
        46, 47, 48
    };
    const Index indices[] = { 0, 1, 0, 0, 1, 2, 2, 1, 0, 0, 0, 0, 0, 0, 0 };
    const size_t vertexStarts[] = { 0, 8, 20 };
    const size_t vertexSizes[] = { 8, 12, 4 };
    const size_t depthStarts[] = { 0, 6, 15 };
    const size_t depthSizes[] = { 6, 9, 3 };
    const uint32_t baseVertices[] = { 0, 2, 5 };
    const uint32_t startIndices[] = { 0, 3, 9 };
    const uint32_t indexCounts[] = { 3, 6, 6 };

    // 사용 영역 뒤를 sentinel로 채워 16-bit index 정렬 여유 공간의 침범도 검출한다.
    std::array<uint8_t, 80> vb, depthVB, ib;
    vb.fill(0xCD);
    depthVB.fill(0xCD);
    ib.fill(0xCD);
    Renderer::MeshBufferPacking packing;

    // converter가 사용하는 동일 Append 경로에서 byte 위치와 원소 위치를 검사한다.
    for (size_t i = 0; i < 3; ++i)
    {
        const auto location = packing.Append(vb.data(), depthVB.data(), ib.data(),
            vertices + vertexStarts[i], vertexSizes[i], 4,
            depthVertices + depthStarts[i], depthSizes[i],
            indices + startIndices[i], indexCounts[i] * sizeof(Index), sizeof(Index));
        Require(location.BaseVertex == baseVertices[i], "Wrong base vertex");
        Require(location.StartIndex == startIndices[i], "Wrong start index");
    }

    // 최종 byte열을 독립적인 고정 기대값과 비교해 겹침과 depth 덮어쓰기를 검출한다.
    Require(std::memcmp(vb.data(), vertices, sizeof(vertices)) == 0, "Vertex stream differs");
    Require(std::memcmp(depthVB.data(), depthVertices, sizeof(depthVertices)) == 0, "Depth stream differs");
    Require(std::memcmp(ib.data(), indices, sizeof(indices)) == 0, "Index stream differs");
    Require(packing.VertexBytes == 24 && packing.DepthVertexBytes == 18, "Wrong vertex byte counts");
    Require(packing.IndexBytes == sizeof(indices), "Wrong index byte count");
    Require(packing.VertexCount == 6 && packing.IndexCount == 15, "Wrong element counts");
    for (size_t i = sizeof(vertices); i < vb.size(); ++i)
        Require(vb[i] == 0xCD, "Vertex stream overrun");
    for (size_t i = sizeof(depthVertices); i < depthVB.size(); ++i)
        Require(depthVB[i] == 0xCD, "Depth stream overrun");
    for (size_t i = sizeof(indices); i < ib.size(); ++i)
        Require(ib[i] == 0xCD, "Index stream overrun or padding overwritten");
}

// GPU나 모델 asset 없이 16/32-bit primitive 병합 회귀 검사를 실행한다.
int main()
{
    // 두 index 형식에서 동일한 vertex/depth stream과 draw 단위 계약을 확인한다.
    try
    {
        CheckPacking<uint16_t>();
        CheckPacking<uint32_t>();
        std::puts("MeshBufferPackingTests passed (16/32-bit, three primitives)");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
