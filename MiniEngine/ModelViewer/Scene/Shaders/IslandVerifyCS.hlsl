#include "IslandUnion.hlsli"

// 모든 활성 연결의 root 일치와 최종 부모 압축 상태를 검증한다.
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 추출된 삼각형만 검증한다.
    uint t=id.x+id.y*(65535*64);
    if(t>=Capacity || t>=Meta.Load(0)) return;

    // 직접 부모가 동일한 최종 root가 아니면 K 한도 fallback을 요청한다.
    for(uint side=0;side<2;side++)
    {
        uint3 nodes=SideNodes(t,side);
        uint first=0xffffffff;
        [unroll] for(uint k=0;k<3;k++) if(nodes[k]!=0xffffffff)
        {
            uint root=Parent[nodes[k]];
            if(Parent[root]!=root || (first!=0xffffffff && first!=root)) Status.InterlockedOr(0,1);
            first=root;
        }
    }
}
