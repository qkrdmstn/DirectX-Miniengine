#include "CutCommon.hlsli"
groupshared uint3 Minimum[64],Maximum[64];
// 정점마다 전역 atomic을 수행하지 않고 그룹별 축소 후 정확한 결과 AABB를 합친다.
[numthreads(64,1,1)]
void main(uint3 id:SV_DispatchThreadID,uint tid:SV_GroupIndex)
{
    uint vertex=id.x,section=0;
    while(section+1<Sections && vertex>=Inputs[section+1].VertexStart)section++;
    Section s=Inputs[section];
    [unroll]for(uint side=0;side<2;side++)
    {
        uint count=Meta.Load(80+(side*Sections+section)*4);
        bool valid=vertex<VertexCapacity&&vertex>=s.VertexStart&&vertex-s.VertexStart<min(count,s.VertexCapacity);
        float3 p=0;
        if(valid)p=side==0?asfloat(PoseA.Load3(vertex*40)):asfloat(PoseB.Load3(vertex*40));
        uint3 ordered=uint3(Ordered(p.x),Ordered(p.y),Ordered(p.z));
        Minimum[tid]=valid?ordered:uint3(0xffffffff,0xffffffff,0xffffffff);
        Maximum[tid]=valid?ordered:uint3(0,0,0);
        GroupMemoryBarrierWithGroupSync();
        for(uint offset=32;offset>0;offset/=2)
        {
            if(tid<offset){Minimum[tid]=min(Minimum[tid],Minimum[tid+offset]);Maximum[tid]=max(Maximum[tid],Maximum[tid+offset]);}
            GroupMemoryBarrierWithGroupSync();
        }
        if(tid==0)
        {
            uint ignored;
            [unroll]for(uint k=0;k<3;k++){Meta.InterlockedMin(32+side*24+k*4,Minimum[0][k],ignored);Meta.InterlockedMax(44+side*24+k*4,Maximum[0][k],ignored);}
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
