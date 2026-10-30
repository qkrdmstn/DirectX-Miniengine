#include "CutCommon.hlsli"

#ifndef CAP_THREADS
#define CAP_THREADS 256
#endif
groupshared uint Remaining,Removed;
groupshared float Orientation;
groupshared uint Cached,LoopLeader,AllPoints;
groupshared float2 CachedPoints[1024];
groupshared uint CachedNext[1024],CachedPrev[1024],CachedActive[1024],CachedKind[1024],CachedSelected[1024];
cbuffer EarMode : register(b1) { uint SerialMode; };
// 작은 단면은 한 그룹의 공유 메모리에 현재 경계를 보관한다.
float2 P2(uint i){return Cached!=0?CachedPoints[i]:Point2(i);}
// 현재 경계의 다음 점을 읽는다.
uint Nxt(uint i){return Cached!=0?CachedNext[i]:Next(i);}
// 현재 경계의 이전 점을 읽는다.
uint Prv(uint i){return Cached!=0?CachedPrev[i]:Prev(i);}
// 현재 loop의 활성 점인지 확인한다.
uint Active(uint i){return Cached!=0?CachedActive[i]:(Ring.Load(i*128+48)==LoopLeader?Ring.Load(i*128+80):0);}
// 현재 라운드의 귀/공선 후보를 읽는다.
uint Kind(uint i){return Cached!=0?CachedKind[i]:Ring.Load(i*128+60);}
// reflex 검사 또는 확정 제거 마스크를 읽는다.
uint Selected(uint i){return Cached!=0?CachedSelected[i]:Ring.Load(i*128+88);}


// 슬롯 배정 순서와 무관하게 투영 위치 비트와 라운드로 우선순위를 섞는다.
uint Priority(uint2 key,uint round)
{
    // 두 좌표 비트를 결합한 뒤 라운드별로 분산한다.
    uint h=key.x+0x9e3779b9u;h^=key.y+0x9e3779b9u+(h<<6)+(h>>2);
    h+=0x9e3779b9u*(round+1);h^=h>>16;h*=0x7feb352du;h^=h>>15;h*=0x846ca68bu;return h^(h>>16);
}
// 우선순위 동률은 위치 비트 사전순으로 풀고 같은 위치에서만 슬롯 ID를 쓴다.
bool PriorityLess(uint id,uint priority,uint other,uint otherPriority)
{
    // 해시가 다르면 위치를 다시 읽지 않고 비교한다.
    if(priority!=otherPriority)return priority<otherPriority;

    // 위치 비트까지 같을 때만 슬롯 순서로 최종 동률을 푼다.
    uint2 key=asuint(P2(id)),otherKey=asuint(P2(other));
    return key.x<otherKey.x || (key.x==otherKey.x && (key.y<otherKey.y || (key.y==otherKey.y && id<other)));
}
// 경계에 닿는 점도 귀 안에 있다고 판정해 잘못된 대각선을 만들지 않는다.
bool InTriangle(float2 p,float2 a,float2 b,float2 c,float sign)
{
    return Cross2(b-a,p-a)*sign>=-Epsilon*Epsilon && Cross2(c-b,p-b)*sign>=-Epsilon*Epsilon
        && Cross2(a-c,p-c)*sign>=-Epsilon*Epsilon;
}
// 한 경계에 한 그룹을 배정하고 그룹 내부에서 귀 검사와 안전한 다중 제거를 병렬 수행한다.
[numthreads(CAP_THREADS,1,1)]
void main(uint3 group:SV_GroupID,uint tid:SV_GroupIndex)
{
    // 유효 loop를 확인하고 그룹 공통 상태를 초기화한다.
    uint leader=group.x+group.y*65535,count=min(Meta.Load(4),MaxSegments);
    if(leader>=count||Ring.Load(leader*128+48)!=leader||(Ring.Load(leader*128+52)&5)!=0)return;
    if(tid==0){Cached=count<=1024?1:0;LoopLeader=leader;AllPoints=(Ring.Load(leader*128+52)&2)!=0?1:0;}
    GroupMemoryBarrierWithGroupSync();

    // 작은 경계를 공유 메모리에 복사한다.
    if(Cached!=0)for(uint cache=tid;cache<count;cache+=CAP_THREADS)
    {
        CachedPoints[cache]=Point2(cache);CachedNext[cache]=Next(cache);CachedPrev[cache]=Prev(cache);
        CachedActive[cache]=Ring.Load(cache*128+48)==leader?1:0;CachedKind[cache]=CachedSelected[cache]=0;
    }
    GroupMemoryBarrierWithGroupSync();

    // 활성 점 수와 방향을 구한다.
    if(tid==0)
    {
        Remaining=0;float area=0;
        for(uint j=0;j<count;j++)if(Active(j)!=0){Remaining++;area+=Cross2(P2(j),P2(Nxt(j)));}
        Orientation=area>=0?1:-1;

        // 아주 큰/퇴화한 단면은 surface 절단을 유지하고 cap만 생략한다.
        if(Remaining>4096||abs(area)<Epsilon*Epsilon)Remaining=0;
    }
    GroupMemoryBarrierWithGroupSync();
    if(Remaining<3)return;

    // 기존 라운드 상한과 선택 규칙으로 귀를 반복 제거한다.
    for(uint round=0;round<min(count,SerialMode==0?256u:4096u) && Remaining>3;round++)
    {
        // 이번 라운드의 제거 수를 초기화한다.
        if(tid==0)Removed=0;
        GroupMemoryBarrierWithGroupSync();

        // 단순 다각형의 귀 내부 검사는 reflex/collinear 정점만 필요하다. Selected는 이 단계에서 임시 reflex mask다.
        for(uint reflex=tid;reflex<count;reflex+=CAP_THREADS)
        {
            if(Active(reflex)==0)continue;
            float turn=Cross2(P2(reflex)-P2(Prv(reflex)),P2(Nxt(reflex))-P2(reflex))*Orientation;
            uint flag=turn<=0?1:0;
            if(Cached!=0)CachedSelected[reflex]=flag;else Ring.Store(reflex*128+88,flag);
        }
        AllMemoryBarrierWithGroupSync();

        // 활성 후보의 귀 또는 공선 여부를 판정한다.
        for(uint i=SerialMode==1?(tid==0?0:count):tid;i<count;i+=SerialMode==1?1:CAP_THREADS)
        {
            if(Active(i)==0)continue;
            uint a=Prv(i),c=Nxt(i);float2 p=P2(a),q=P2(i),r=P2(c);
            float cross=Cross2(q-p,r-q)*Orientation;
            uint kind=0;
            if(abs(cross)<=Epsilon*(length(q-p)+length(r-q)) && dot(q-p,q-r)<=Epsilon*Epsilon)kind=2;
            else if(cross>0)
            {
                kind=1;
                for(uint k=0;k<count;k++)
                    if(k!=a&&k!=i&&k!=c&&Active(k)!=0&&(AllPoints!=0||Selected(k)!=0)
                        && InTriangle(P2(k),p,q,r,Orientation)){kind=0;break;}
            }
            if(Cached!=0)CachedKind[i]=kind;else Ring.Store(i*128+60,kind);
        }
        AllMemoryBarrierWithGroupSync();

        // 제거되는 세 점이 겹치지 않도록 현재 경계의 양쪽 두 이웃과 우선순위를 비교한다.
        for(uint v=tid;v<count;v+=CAP_THREADS)
        {
            if(Active(v)==0)continue;
            if(Kind(v)==0){if(Cached!=0)CachedSelected[v]=0;else Ring.Store(v*128+88,0);continue;}
            uint a=Prv(v),c=Nxt(v);uint neighbors[4]={a,c,Prv(a),Nxt(c)};bool selected=true;
            uint priority=Priority(asuint(P2(v)),round);
            [unroll]for(uint n=0;n<4;n++)
            {
                uint otherPriority=Priority(asuint(P2(neighbors[n])),round);
                if(Kind(neighbors[n])!=0 && PriorityLess(neighbors[n],otherPriority,v,priority))selected=false;
            }
            if(Cached!=0)CachedSelected[v]=selected?1:0;else Ring.Store(v*128+88,selected?1:0);
        }
        AllMemoryBarrierWithGroupSync();

        // 동시에 지워도 최소 세 점은 남도록 잔여가 작은 경우 최저 후보 하나만 고른다.
        if(tid==0 && (Remaining<=5||SerialMode!=0))
        {
            // 병렬 선택을 통과한 후보 중 같은 우선순위 키의 최솟값을 찾는다.
            uint chosen=count,chosenPriority=0;
            for(uint z=0;z<count;z++)if(Active(z)!=0 && Selected(z)!=0)
            {
                uint priority=Priority(asuint(P2(z)),round);
                if(chosen==count || PriorityLess(z,priority,chosen,chosenPriority))
                {chosen=z;chosenPriority=priority;}
            }

            // 최솟값 후보 하나만 남겨 잔여 점 수를 보장한다.
            for(uint clear=0;clear<count;clear++)if(Active(clear)!=0 && Selected(clear)!=0 && clear!=chosen)
            {if(Cached!=0)CachedSelected[clear]=0;else Ring.Store(clear*128+88,0);}
        }
        AllMemoryBarrierWithGroupSync();

        // 선택된 점의 삼각형을 기록하고 연결을 갱신한다.
        for(uint v2=tid;v2<count;v2+=CAP_THREADS)
        {
            if(Active(v2)==0||Selected(v2)==0)continue;
            uint a=Prv(v2),c=Nxt(v2);
            if(Kind(v2)==1){Ring.Store3(v2*128+64,uint3(a,v2,c));Ring.Store(v2*128+76,1);}
            if(Cached!=0){CachedNext[a]=c;CachedPrev[c]=a;CachedActive[v2]=0;}
            else{Ring.Store(a*128+40,c);Ring.Store(c*128+44,a);Ring.Store(v2*128+80,0);}
            InterlockedAdd(Removed,1);
        }
        AllMemoryBarrierWithGroupSync();

        // 제거 수를 반영하고 진행이 없으면 중단한다.
        if(tid==0)Remaining-=Removed;
        GroupMemoryBarrierWithGroupSync();
        if(Removed==0)return;
    }

    // 마지막 세 점을 기록하고 loop 완료를 표시한다.
    if(tid==0 && Remaining==3)
    {
        for(uint i=0;i<count;i++)if(Active(i)!=0)
        {
            Ring.Store3(i*128+64,uint3(i,Nxt(i),Nxt(Nxt(i))));Ring.Store(i*128+76,1);
            Ring.Store(leader*128+84,1);uint ignored;Meta.InterlockedAdd(12,1,ignored);break;
        }
    }
}
