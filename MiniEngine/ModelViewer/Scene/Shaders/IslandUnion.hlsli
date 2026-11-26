cbuffer Parameters : register(b0) { uint Capacity, NodeCount, Initialize, Padding; };
ByteAddressBuffer Meta : register(t0);
StructuredBuffer<float3> CornerDistance : register(t1);
StructuredBuffer<uint3> CornerCell : register(t2);
RWStructuredBuffer<uint> Parent : register(u0);
RWByteAddressBuffer Status : register(u1);

// CPU의 side 존재 규칙과 영점 코너 포함 규칙으로 노드를 고른다.
uint3 SideNodes(uint t, uint side)
{
    // 결정 5의 이동 항은 상쇄되므로 d0를 직접 분류한다.
    float3 d=CornerDistance[t];
    bool positive=any(d>1e-5), negative=any(d < -1e-5);
    uint3 nodes=uint3(0xffffffff,0xffffffff,0xffffffff);
    if(side==0 ? !positive && negative : !negative) return nodes;

    // 영점은 존재하는 양쪽 side의 코너 집합에 속한다.
    [unroll] for(uint k=0;k<3;k++)
        if(side==0 ? d[k]>=-1e-5 : d[k]<=1e-5) nodes[k]=2*CornerCell[t][k]+side;
    return nodes;
}

// 동시 hook 중에도 감소하는 부모를 원자적으로 읽어 현재 root를 찾는다.
uint Root(uint node)
{
    // 부모는 항상 감소하므로 순환 없이 종료한다.
    uint next;
    for(;;)
    {
        InterlockedAdd(Parent[node],0,next);
        if(next==node)
        {
            return node;
        }
        node=next;
    }
}
