#include "CutCommon.hlsli"
cbuffer Pass : register(b1) { uint Commit; };
// 읽기/쓰기 dispatch를 분리한 pointer jumping으로 한 경계 안의 대표와 열린 상태를 전파한다.
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID)
{
    uint i=id.x,count=min(Meta.Load(4),MaxSegments);if(i>=count)return;
    uint a=i*128;
    if(Commit==0)
    {
        uint j=Ring.Load(a+56)*128;
        Ring.Store(a+96,min(Ring.Load(a+48),Ring.Load(j+48)));
        Ring.Store(a+100,Ring.Load(a+52)|Ring.Load(j+52));
        Ring.Store(a+104,Ring.Load(j+56));
    }
    else
    {
        Ring.Store(a+48,Ring.Load(a+96));Ring.Store(a+52,Ring.Load(a+100));Ring.Store(a+56,Ring.Load(a+104));
    }
}
