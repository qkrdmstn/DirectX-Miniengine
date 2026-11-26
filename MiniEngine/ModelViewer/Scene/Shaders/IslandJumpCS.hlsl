#include "IslandUnion.hlsli"

// 최초 부모를 초기화하거나 부모의 부모로 경로를 압축한다.
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 노드마다 하나의 초기화 또는 pointer jump를 기록한다.
    uint node=id.x+id.y*(65535*64);
    if(node>=NodeCount)
    {
        return;
    }
    if(Initialize!=0)
    {
        Parent[node]=node;
        if(node==0)
        {
            Status.Store(0,0);
        }
        return;
    }

    // 원자적 최소값으로 다른 스레드의 부모 감소를 보존한다.
    uint parent, grandparent;
    InterlockedAdd(Parent[node],0,parent);
    InterlockedAdd(Parent[parent],0,grandparent);
    InterlockedMin(Parent[node],grandparent);
}
