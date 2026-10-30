#include "CutCommon.hlsli"

#ifndef CAP_THREADS
#define CAP_THREADS 256
#endif
groupshared uint Remaining,Removed;
groupshared float Orientation;
groupshared uint Cached,LoopLeader,AllPoints,LoopLength,Chosen;
groupshared float2 CachedPoints[1024];
groupshared uint CachedNext[1024],CachedPrev[1024],CachedFlags[1024],CachedSlot[1024];
cbuffer EarMode : register(b1) { uint SerialMode; };
// 작은 단면은 한 그룹의 공유 메모리에 현재 경계를 보관한다.
float2 P2(uint i)
{
    // 캐시 위치 또는 원래 슬롯의 좌표를 반환한다.
    return Cached!=0?CachedPoints[i]:Point2(i);
}
// 현재 경계의 다음 점을 읽는다.
uint Nxt(uint i)
{
    // 캐시 위치 또는 원래 슬롯의 연결을 반환한다.
    return Cached!=0?CachedNext[i]:Next(i);
}
// 현재 경계의 이전 점을 읽는다.
uint Prv(uint i)
{
    // 캐시 위치 또는 원래 슬롯의 연결을 반환한다.
    return Cached!=0?CachedPrev[i]:Prev(i);
}
// 현재 loop의 활성 점인지 확인한다.
uint Active(uint i)
{
    // 활성 비트 또는 현재 loop의 슬롯 상태를 반환한다.
    return Cached!=0?(CachedFlags[i]&1):(Ring.Load(i*128+48)==LoopLeader?Ring.Load(i*128+80):0);
}
// 현재 라운드의 귀/공선 후보를 읽는다.
uint Kind(uint i)
{
    // 후보 종류 두 비트 또는 슬롯의 후보 종류를 반환한다.
    return Cached!=0?((CachedFlags[i]>>1)&3):Ring.Load(i*128+60);
}
// reflex 검사 또는 확정 제거 마스크를 읽는다.
uint Selected(uint i)
{
    // 선택 비트 또는 슬롯의 선택 상태를 반환한다.
    return Cached!=0?((CachedFlags[i]>>3)&1):Ring.Load(i*128+88);
}
// 캐시 위치를 Ring 기록과 동률 비교에 사용할 원래 슬롯으로 바꾼다.
uint Slot(uint i)
{
    // 비캐시 경로의 슬롯 번호는 그대로 유지한다.
    return Cached!=0?CachedSlot[i]:i;
}


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
    return key.x<otherKey.x || (key.x==otherKey.x && (key.y<otherKey.y || (key.y==otherKey.y && Slot(id)<Slot(other))));
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
    if(tid==0)
    {
        // leader부터 닫힌 경계를 검증하고 처음 1024개 위치의 슬롯을 보관한다.
        LoopLeader=leader;
        AllPoints=(Ring.Load(leader*128+52)&2)!=0?1:0;
        LoopLength=0;
        uint slot=leader;
        for(uint p=0;p<min(count,4096u);p++)
        {
            if(slot>=count)break;
            if(Ring.Load(slot*128+48)!=leader)break;
            if(p<1024)CachedSlot[p]=slot;
            slot=Next(slot);
            if(slot==leader)
            {
                LoopLength=p+1;
                break;
            }
        }

        // 순회 실패는 그룹 전체가 cap을 생략하도록 전달한다.
        Cached=LoopLength<=1024?1:0;
    }
    GroupMemoryBarrierWithGroupSync();
    if(LoopLength<3)return;
    uint size=Cached!=0?LoopLength:count;

    // 작은 경계를 공유 메모리에 복사한다.
    if(Cached!=0)for(uint cache=tid;cache<size;cache+=CAP_THREADS)
    {
        // 각 위치의 소유 스레드가 좌표와 연결 및 압축 플래그를 초기화한다.
        CachedPoints[cache]=Point2(CachedSlot[cache]);
        CachedNext[cache]=(cache+1)%size;
        CachedPrev[cache]=(cache+size-1)%size;
        CachedFlags[cache]=1;
    }
    GroupMemoryBarrierWithGroupSync();

    // 활성 점 수와 방향을 구한다.
    if(tid==0)
    {
        Remaining=0;
        float area=0;
        for(uint j=0;j<size;j++)if(Active(j)!=0)
        {
            Remaining++;
            area+=Cross2(P2(j),P2(Nxt(j)));
        }
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
        for(uint reflex=tid;reflex<size;reflex+=CAP_THREADS)
        {
            if(Active(reflex)==0)continue;
            float turn=Cross2(P2(reflex)-P2(Prv(reflex)),P2(Nxt(reflex))-P2(reflex))*Orientation;
            uint flag=turn<=0?1:0;
            if(Cached!=0)CachedFlags[reflex]=(CachedFlags[reflex]&~8u)|(flag<<3);
            else Ring.Store(reflex*128+88,flag);
        }
        AllMemoryBarrierWithGroupSync();

        // 활성 후보의 귀 또는 공선 여부를 판정한다.
        for(uint i=Cached==0 && SerialMode==1?(tid==0?0:size):tid;i<size;i+=Cached==0 && SerialMode==1?1:CAP_THREADS)
        {
            if(Active(i)==0)continue;
            uint a=Prv(i),c=Nxt(i);float2 p=P2(a),q=P2(i),r=P2(c);
            float cross=Cross2(q-p,r-q)*Orientation;
            uint kind=0;
            if(abs(cross)<=Epsilon*(length(q-p)+length(r-q)) && dot(q-p,q-r)<=Epsilon*Epsilon)kind=2;
            else if(cross>0)
            {
                kind=1;
                for(uint k=0;k<size;k++)
                    if(k!=a&&k!=i&&k!=c&&Active(k)!=0&&(AllPoints!=0||Selected(k)!=0)
                        && InTriangle(P2(k),p,q,r,Orientation)){kind=0;break;}
            }
            if(Cached!=0)CachedFlags[i]=(CachedFlags[i]&~6u)|(kind<<1);
            else Ring.Store(i*128+60,kind);
        }
        AllMemoryBarrierWithGroupSync();

        // 제거되는 세 점이 겹치지 않도록 현재 경계의 양쪽 두 이웃과 우선순위를 비교한다.
        for(uint v=tid;v<size;v+=CAP_THREADS)
        {
            if(Active(v)==0)continue;
            if(Kind(v)==0)
            {
                if(Cached!=0)CachedFlags[v]&=~8u;
                else Ring.Store(v*128+88,0);
                continue;
            }
            uint a=Prv(v),c=Nxt(v);uint neighbors[4]={a,c,Prv(a),Nxt(c)};bool selected=true;
            uint priority=Priority(asuint(P2(v)),round);
            [unroll]for(uint n=0;n<4;n++)
            {
                uint otherPriority=Priority(asuint(P2(neighbors[n])),round);
                if(Kind(neighbors[n])!=0 && PriorityLess(neighbors[n],otherPriority,v,priority))selected=false;
            }
            if(Cached!=0)CachedFlags[v]=(CachedFlags[v]&~8u)|(selected?8u:0u);
            else Ring.Store(v*128+88,selected?1:0);
        }
        AllMemoryBarrierWithGroupSync();

        // 동시에 지워도 최소 세 점은 남도록 잔여가 작은 경우 최저 후보 하나만 고른다.
        if(tid==0 && (Remaining<=5||SerialMode!=0))
        {
            // 병렬 선택을 통과한 후보 중 같은 우선순위 키의 최솟값을 찾는다.
            uint chosen=size,chosenPriority=0;
            for(uint z=0;z<size;z++)if(Active(z)!=0 && Selected(z)!=0)
            {
                uint priority=Priority(asuint(P2(z)),round);
                if(chosen==size || PriorityLess(z,priority,chosen,chosenPriority))
                {
                    chosen=z;
                    chosenPriority=priority;
                }
            }

            // 소유 스레드가 선택 비트를 정리하도록 최솟값 위치를 공유한다.
            Chosen=chosen;
        }

        // 각 위치의 소유 스레드만 압축 word를 수정한다.
        if(Remaining<=5||SerialMode!=0)
        {
            GroupMemoryBarrierWithGroupSync();
            for(uint clear=tid;clear<size;clear+=CAP_THREADS)
                if(Active(clear)!=0 && Selected(clear)!=0 && clear!=Chosen)
                {
                    if(Cached!=0)CachedFlags[clear]&=~8u;
                    else Ring.Store(clear*128+88,0);
                }
        }
        AllMemoryBarrierWithGroupSync();

        // 선택된 점의 삼각형을 기록하고 연결을 갱신한다.
        for(uint v2=tid;v2<size;v2+=CAP_THREADS)
        {
            if(Active(v2)==0||Selected(v2)==0)continue;
            uint a=Prv(v2),c=Nxt(v2);
            if(Kind(v2)==1)
            {
                Ring.Store3(Slot(v2)*128+64,uint3(Slot(a),Slot(v2),Slot(c)));
                Ring.Store(Slot(v2)*128+76,1);
            }

            // 이웃 연결은 각각 갱신하고 자기 위치의 활성 비트만 내린다.
            if(Cached!=0)
            {
                CachedNext[a]=c;
                CachedPrev[c]=a;
                CachedFlags[v2]&=~1u;
            }
            else
            {
                Ring.Store(a*128+40,c);
                Ring.Store(c*128+44,a);
                Ring.Store(v2*128+80,0);
            }
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
        // 마지막 세 점도 원래 최소 슬롯을 시작점으로 삼는다.
        uint first=size;
        for(uint i=0;i<size;i++)if(Active(i)!=0)
            if(first==size || Slot(i)<Slot(first))first=i;
        if(first!=size)
        {
            Ring.Store3(Slot(first)*128+64,uint3(Slot(first),Slot(Nxt(first)),Slot(Nxt(Nxt(first)))));
            Ring.Store(Slot(first)*128+76,1);
            Ring.Store(leader*128+84,1);
            uint ignored;
            Meta.InterlockedAdd(12,1,ignored);
        }
    }
}
