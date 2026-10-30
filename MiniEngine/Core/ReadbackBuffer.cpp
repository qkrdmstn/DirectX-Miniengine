//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author:  James Stanard 
//

#include "pch.h"
#include "ReadbackBuffer.h"
#include "GraphicsCore.h"
#include <stdexcept>

using namespace Graphics;

// CPU 읽기용 버퍼를 생성하고 GPU 할당 실패를 호출자에게 전달한다.
void ReadbackBuffer::Create( const std::wstring& name, uint32_t NumElements, uint32_t ElementSize )
{
    // 이전 자원을 해제하고 byte 크기의 32비트 곱 오버플로를 피한다.
    Destroy();

    m_ElementCount = NumElements;
    m_ElementSize = ElementSize;
    m_BufferSize = size_t(NumElements) * ElementSize;
    m_UsageState = D3D12_RESOURCE_STATE_COPY_DEST;

    // CPU 읽기가 가능한 heap을 선택한다.
    D3D12_HEAP_PROPERTIES HeapProps;
    HeapProps.Type = D3D12_HEAP_TYPE_READBACK;
    HeapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    HeapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    HeapProps.CreationNodeMask = 1;
    HeapProps.VisibleNodeMask = 1;

    // 읽기 버퍼를 1차원 선형 자원으로 정의한다.
    D3D12_RESOURCE_DESC ResourceDesc = {};
    ResourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ResourceDesc.Width = m_BufferSize;
    ResourceDesc.Height = 1;
    ResourceDesc.DepthOrArraySize = 1;
    ResourceDesc.MipLevels = 1;
    ResourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    ResourceDesc.SampleDesc.Count = 1;
    ResourceDesc.SampleDesc.Quality = 0;
    ResourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ResourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    // 실패한 자원을 참조하기 전에 예외를 전달한다.
    const HRESULT result = g_Device->CreateCommittedResource(&HeapProps, D3D12_HEAP_FLAG_NONE, &ResourceDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, MY_IID_PPV_ARGS(&m_pResource));
    if (FAILED(result))
        throw std::runtime_error("GPU readback allocation failed, HRESULT=" + std::to_string(uint32_t(result)));

    // 성공한 자원의 주소와 개발용 이름을 저장한다.
    m_GpuVirtualAddress = m_pResource->GetGPUVirtualAddress();

#ifdef RELEASE
    (name);
#else
    m_pResource->SetName(name.c_str());
#endif
}


// 읽기 매핑 실패나 빈 자원에서 유효하지 않은 CPU 포인터를 반환하지 않는다.
void* ReadbackBuffer::Map(void)
{
    // 유효한 자원만 매핑하고 실패 HRESULT를 명시적으로 전달한다.
    if (!m_pResource)
        throw std::runtime_error("Cannot map an empty GPU readback buffer");
    void* Memory = nullptr;
    const HRESULT result = m_pResource->Map(0, &CD3DX12_RANGE(0, m_BufferSize), &Memory);
    if (FAILED(result))
        throw std::runtime_error("GPU readback map failed, HRESULT=" + std::to_string(uint32_t(result)));

    // 성공한 읽기 영역만 호출자에게 넘긴다.
    return Memory;
}

void ReadbackBuffer::Unmap(void)
{
    m_pResource->Unmap(0, &CD3DX12_RANGE(0, 0));
}
