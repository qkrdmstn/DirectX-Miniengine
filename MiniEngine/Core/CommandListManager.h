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

#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <stdint.h>
#include <array>
#include <wrl/client.h>
#include "CommandAllocatorPool.h"

class CommandQueue
{
    friend class CommandListManager;
    friend class CommandContext;

public:
    CommandQueue(D3D12_COMMAND_LIST_TYPE Type);
    ~CommandQueue();

    void Create(ID3D12Device* pDevice);
    void Shutdown();

    inline bool IsReady()
    {
        return m_CommandQueue != nullptr;
    }

    uint64_t IncrementFence(void);
    bool IsFenceComplete(uint64_t FenceValue);
    void StallForFence(uint64_t FenceValue);
    void StallForProducer(CommandQueue& Producer);
    void WaitForFence(uint64_t FenceValue);
    void WaitForIdle(void) { WaitForFence(IncrementFence()); }

    ID3D12CommandQueue* GetCommandQueue() { return m_CommandQueue; }

    uint64_t GetNextFenceValue() { return m_NextFenceValue; }

private:

    uint64_t ExecuteCommandList(ID3D12CommandList* List);
    ID3D12CommandAllocator* RequestAllocator(void);
    void DiscardAllocator(uint64_t FenceValueForReset, ID3D12CommandAllocator* Allocator);

    ID3D12CommandQueue* m_CommandQueue;

    const D3D12_COMMAND_LIST_TYPE m_Type;

    CommandAllocatorPool m_AllocatorPool;
    std::mutex m_FenceMutex;
    std::mutex m_EventMutex;

    // Lifetime of these objects is managed by the descriptor cache
    ID3D12Fence* m_pFence;
    uint64_t m_NextFenceValue;
    uint64_t m_LastCompletedFenceValue;
    HANDLE m_FenceEventHandle;

};

class CommandListManager
{
    friend class CommandContext;

public:
    CommandListManager();
    ~CommandListManager();

    void Create(ID3D12Device* pDevice);
    void Shutdown();

    CommandQueue& GetGraphicsQueue(void) { return m_GraphicsQueue; }
    CommandQueue& GetComputeQueue(void) { return m_ComputeQueue; }
    CommandQueue& GetCopyQueue(void) { return m_CopyQueue; }

    CommandQueue& GetQueue(D3D12_COMMAND_LIST_TYPE Type = D3D12_COMMAND_LIST_TYPE_DIRECT)
    {
        switch (Type)
        {
        case D3D12_COMMAND_LIST_TYPE_COMPUTE: return m_ComputeQueue;
        case D3D12_COMMAND_LIST_TYPE_COPY: return m_CopyQueue;
        default: return m_GraphicsQueue;
        }
    }

    ID3D12CommandQueue* GetCommandQueue()
    {
        return m_GraphicsQueue.GetCommandQueue();
    }

    void CreateNewCommandList(
        D3D12_COMMAND_LIST_TYPE Type,
        ID3D12GraphicsCommandList** List,
        ID3D12CommandAllocator** Allocator);

    // Test to see if a fence has already been reached
    bool IsFenceComplete(uint64_t FenceValue)
    {
        return GetQueue(D3D12_COMMAND_LIST_TYPE(FenceValue >> 56)).IsFenceComplete(FenceValue);
    }

    // The CPU will wait for a fence to reach a specified value
    void WaitForFence(uint64_t FenceValue);

    // The CPU will wait for all command queues to empty (so that the GPU is idle)
    void IdleGPU(void)
    {
        m_GraphicsQueue.WaitForIdle();
        m_ComputeQueue.WaitForIdle();
        m_CopyQueue.WaitForIdle();
    }

    // scene thread 전용이며 queue 제출과 동시에 호출하지 않는다.
    // 제출된 명령의 resource 참조를 queue fence가 완료될 때까지 보관한다. 미제출 명령은 호출자가 먼저 제출해야 한다.
    void RetireResource(ID3D12Resource* resource) noexcept;
    // scene thread 전용이며 queue 제출과 동시에 호출하지 않는다.
    // 완료된 retired resource만 대기 없이 해제한다.
    void CollectRetiredResources() noexcept;
    // scene thread에서 보관 중인 retired resource 수를 반환한다.
    size_t GetRetiredResourceCount() const noexcept
    {
        // scene thread에서 보관 중인 retired resource 수를 반환한다.
        return m_RetiredResources.size();
    }
    // scene thread에서 보관 중인 buffer resource의 논리 byte 합을 반환한다.
    uint64_t GetRetiredResourceBytes() const noexcept
    {
        // scene thread에서 보관 중인 buffer resource의 논리 Width 합을 반환한다.
        return m_RetiredResourceBytes;
    }

private:

    struct RetiredResource
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
        std::array<uint64_t, 3> FenceValues;
        uint64_t Bytes;
    };

    // retired 목록에 필요한 queue fence가 모두 끝났는지 확인한다.
    bool IsRetiredResourceComplete(const RetiredResource& retired) noexcept;
    // allocation 실패 시 resource 소유권이 원본 wrapper에 남아 있는 동안 GPU를 완료시킨다.
    void WaitForReadyQueues() noexcept;

    ID3D12Device* m_Device;

    CommandQueue m_GraphicsQueue;
    CommandQueue m_ComputeQueue;
    CommandQueue m_CopyQueue;
    std::vector<RetiredResource> m_RetiredResources;
    uint64_t m_RetiredResourceBytes = 0;
};
