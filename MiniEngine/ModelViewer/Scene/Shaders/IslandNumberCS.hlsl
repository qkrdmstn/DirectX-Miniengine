#include "IslandUnion.hlsli"
RWStructuredBuffer<uint> RootMin : register(u2);
RWStructuredBuffer<uint> Prefix : register(u3);
RWStructuredBuffer<uint2> TriangleSideIsland : register(u4);
groupshared uint Scan[256];

// 존재하는 side의 최종 root를 반환하고 없는 side는 무효값으로 둔다.
uint SideRoot(uint key)
{
    // 유효 코너 하나의 압축된 부모가 성분을 대표한다.
    uint3 nodes = SideNodes(key / 2, key % 2);
    for (uint k = 0; k < 3; ++k)
    {
        if (nodes[k] != 0xffffffff)
        {
            return Parent[nodes[k]];
        }
    }
    return 0xffffffff;
}

// 초기화·최소 키·블록 scan·블록 합계 scan·번호 기록을 같은 셰이더로 수행한다.
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint lane : SV_GroupIndex)
{
    // 미수렴 요청은 어떤 번호 부여 작업도 실행하지 않는다.
    if (Status.Load(0) != 0)
    {
        return;
    }
    uint key = id.x;
    uint keys = Meta.Load(0) * 2;
    uint blocks = (keys + 255) / 256;
    uint blockBase = Capacity * 2;

    // 노드별 최소 키와 최종 섬 개수를 초기화한다.
    if (Initialize == 0)
    {
        if (key < NodeCount)
        {
            RootMin[key] = 0xffffffff;
        }
        if (key == 0)
        {
            Status.Store(4, 0);
        }
        return;
    }

    // 작은 블록 합계 배열을 배타적 scan하여 전체 섬 개수를 기록한다.
    if (Initialize == 3)
    {
        if (key == 0)
        {
            uint sum = 0;
            for (uint b = 0; b < blocks; ++b)
            {
                uint value = Prefix[blockBase + b];
                Prefix[blockBase + b] = sum;
                sum += value;
            }
            Status.Store(4, sum);
        }
        return;
    }

    // 활성 키만 root에 최소 키를 원자적으로 누적한다.
    uint root = 0xffffffff;
    if (key < keys)
    {
        root = SideRoot(key);
    }
    if (Initialize == 1)
    {
        if (root != 0xffffffff)
        {
            InterlockedMin(RootMin[root], key);
        }
        return;
    }

    // 첫 등장 head 플래그의 블록별 포함 scan을 동기화하며 계산한다.
    if (Initialize == 2)
    {
        uint head = 0;
        if (root != 0xffffffff)
        {
            head = RootMin[root] == key ? 1 : 0;
        }
        Scan[lane] = head;
        GroupMemoryBarrierWithGroupSync();
        for (uint stride = 1; stride < 256; stride *= 2)
        {
            uint addend = lane >= stride ? Scan[lane - stride] : 0;
            GroupMemoryBarrierWithGroupSync();
            Scan[lane] += addend;
            GroupMemoryBarrierWithGroupSync();
        }
        if (key < keys)
        {
            Prefix[key] = Scan[lane];
        }
        if (lane == 255 && key / 256 < blocks)
        {
            Prefix[blockBase + key / 256] = Scan[lane];
        }
        return;
    }

    // 삼각형마다 양쪽 번호를 함께 써서 uint2의 동시 쓰기를 피한다.
    if (key < Meta.Load(0))
    {
        uint2 islands = uint2(0xffffffff, 0xffffffff);
        for (uint side = 0; side < 2; ++side)
        {
            uint sideRoot = SideRoot(key * 2 + side);
            if (sideRoot != 0xffffffff)
            {
                uint first = RootMin[sideRoot];
                islands[side] = Prefix[first] + Prefix[blockBase + first / 256] - 1;
            }
        }
        TriangleSideIsland[key] = islands;
    }
}
