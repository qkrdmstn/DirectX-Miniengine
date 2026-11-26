#include "IslandUnion.hlsli"

// 삼각형의 존재하는 side마다 코너의 현재 root를 작은 root로 연결한다.
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 추출 카운터 밖 슬롯은 읽지 않는다.
    uint t=id.x+id.y*(65535*64);
    if(t>=Capacity || t>=Meta.Load(0))
    {
        return;
    }

    // 모든 유효 코너 root에 최소 root hook을 적용한다.
    for(uint side=0;side<2;side++)
    {
        uint3 nodes=SideNodes(t,side), roots=nodes;
        uint lowest=0xffffffff;
        [unroll] for(uint k=0;k<3;k++)
        {
            if(nodes[k]!=0xffffffff)
            {
                roots[k]=Root(nodes[k]);
                lowest=min(lowest,roots[k]);
            }
        }
        [unroll] for(uint j=0;j<3;j++)
        {
            if(nodes[j]!=0xffffffff)
            {
                InterlockedMin(Parent[roots[j]],lowest);
            }
        }
    }
}
