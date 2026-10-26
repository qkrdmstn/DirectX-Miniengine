#include "CutCommon.hlsli"
groupshared uint Remaining,Removed;
groupshared float Orientation;
groupshared uint Cached,LoopLeader,AllPoints;
groupshared uint FinishSerial;
groupshared float2 CachedPoints[1024];
groupshared uint CachedNext[1024],CachedPrev[1024],CachedActive[1024],CachedKind[1024],CachedSelected[1024];
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
// ear 내부 검사에 사용할 reflex/공선 정점 마스크를 읽는다.
uint Selected(uint i){return Cached!=0?CachedSelected[i]:Ring.Load(i*128+88);}
// 경계에 닿는 점도 귀 안에 있다고 판정해 잘못된 대각선을 만들지 않는다.
bool InTriangle(float2 p,float2 a,float2 b,float2 c,float sign)
{
    return Cross2(b-a,p-a)*sign>=-Epsilon*Epsilon && Cross2(c-b,p-b)*sign>=-Epsilon*Epsilon
        && Cross2(a-c,p-c)*sign>=-Epsilon*Epsilon;
}
// 현재 경계를 순회하며 볼록 후보를 하나 건너 표시한다. 공선 제거 후보도 같은 간격 규칙을 따른다.
void MarkCandidates(uint count)
{
    // 제거된 대표 대신 현재 활성 경계의 시작점을 찾는다.
    uint start=0;
    while(start<count && Active(start)==0)start++;
    if(start==count)return;

    // 원래 ID의 홀짝이 아니라 현재 경계에서 후보가 나타나는 순서로 교대 선택한다.
    uint v=start,first=count,last=count;
    bool take=true;
    do
    {
        float2 p=P2(Prv(v)),q=P2(v),r=P2(Nxt(v));
        float cross=Cross2(q-p,r-q)*Orientation;
        bool collinear=abs(cross)<=Epsilon*(length(q-p)+length(r-q)) && dot(q-p,q-r)<=Epsilon*Epsilon;
        if(cross>0 || collinear)
        {
            if(take)
            {
                if(Cached!=0)CachedKind[v]=3;else Ring.Store(v*128+60,3);
                if(first==count)first=v;
                last=v;
            }
            take=!take;
        }
        v=Nxt(v);
    }while(v!=start);

    // 순환 경계의 마지막 후보와 첫 후보가 인접하면 마지막 표시를 취소한다.
    if(last!=first && last<count && Nxt(last)==first)
    {
        if(Cached!=0)CachedKind[last]=0;else Ring.Store(last*128+60,0);
    }
}
// 한 경계에 한 그룹을 배정하고 그룹 내부에서 귀 검사와 안전한 다중 제거를 병렬 수행한다.
[numthreads(64,1,1)]
void main(uint3 group:SV_GroupID,uint tid:SV_GroupIndex)
{
    // 이 그룹에 대응하는 유효한 경계와 공유 상태를 준비한다.
    uint leader=group.x+group.y*65535,count=min(Meta.Load(4),MaxSegments);
    if(leader>=count||Ring.Load(leader*128+48)!=leader||(Ring.Load(leader*128+52)&5)!=0)return;
    if(tid==0){Cached=count<=1024?1:0;LoopLeader=leader;AllPoints=(Ring.Load(leader*128+52)&2)!=0?1:0;FinishSerial=0;}
    GroupMemoryBarrierWithGroupSync();
    if(Cached!=0)for(uint cache=tid;cache<count;cache+=64)
    {
        CachedPoints[cache]=Point2(cache);CachedNext[cache]=Next(cache);CachedPrev[cache]=Prev(cache);
        CachedActive[cache]=Ring.Load(cache*128+48)==leader?1:0;CachedKind[cache]=CachedSelected[cache]=0;
    }
    GroupMemoryBarrierWithGroupSync();
    // 현재 경계의 크기와 winding을 구하고 퇴화 입력을 제외한다.
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
    // 선표시 후 검사하며 제거량이 작아지면 GPU 단일 스레드로 마무리한다.
    for(uint round=0;round<min(count,4096u) && Remaining>3;round++)
    {
        if(tid==0)Removed=0;
        GroupMemoryBarrierWithGroupSync();
        // 단순 다각형의 귀 내부 검사는 reflex/collinear 정점만 필요하다. Selected는 이 단계에서 임시 reflex mask다.
        for(uint reflex=tid;reflex<count;reflex+=64)
        {
            if(Active(reflex)==0)continue;
            float turn=Cross2(P2(reflex)-P2(Prv(reflex)),P2(Nxt(reflex))-P2(reflex))*Orientation;
            uint flag=turn<=0?1:0;
            if(Cached!=0)CachedSelected[reflex]=flag;else Ring.Store(reflex*128+88,flag);
            if(Cached!=0)CachedKind[reflex]=0;else Ring.Store(reflex*128+60,0);
        }
        AllMemoryBarrierWithGroupSync();

        // mark-and-cut은 한 스레드가 표시한 후보만 병렬 ear 검사에 넘긴다.
        if(FinishSerial==0 && tid==0)MarkCandidates(count);
        AllMemoryBarrierWithGroupSync();
        bool serialFinish=FinishSerial!=0;
        for(uint i=serialFinish?(tid==0?0:count):tid;i<count;i+=serialFinish?1:64)
        {
            if(Active(i)==0)continue;
            if(!serialFinish && Kind(i)!=3)continue;
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
            if(serialFinish && kind!=0)break;
        }
        AllMemoryBarrierWithGroupSync();

        // 동시에 지워도 최소 세 점은 남도록 잔여가 작은 경우 최저 후보 하나만 고른다.
        if(tid==0 && Remaining<=5)
        {
            bool chosen=false;
            for(uint z=0;z<count;z++)if(Active(z)!=0 && Kind(z)!=0)
            {if(chosen){if(Cached!=0)CachedKind[z]=0;else Ring.Store(z*128+60,0);}chosen=true;}
        }
        AllMemoryBarrierWithGroupSync();
        // 선택된 정점의 고유 슬롯에 삼각형을 기록하고 양방향 연결을 갱신한다.
        for(uint v2=tid;v2<count;v2+=64)
        {
            if(Active(v2)==0||Kind(v2)==0)continue;
            uint a=Prv(v2),c=Nxt(v2);
            if(Kind(v2)==1){Ring.Store3(v2*128+64,uint3(a,v2,c));Ring.Store(v2*128+76,1);}
            if(Cached!=0){CachedNext[a]=c;CachedPrev[c]=a;CachedActive[v2]=0;}
            else{Ring.Store(a*128+40,c);Ring.Store(c*128+44,a);Ring.Store(v2*128+80,0);}
            InterlockedAdd(Removed,1);
        }
        AllMemoryBarrierWithGroupSync();
        // 논문의 적은 제거량 기준을 적용하되 CPU FIST 대신 GPU 순차 ear clipping으로 끝낸다.
        if(tid==0)
        {
            Remaining-=Removed;
            if(Removed<20)FinishSerial=1;
        }
        GroupMemoryBarrierWithGroupSync();
        if(Removed==0 && serialFinish)return;
    }

    // 남은 세 정점을 마지막 삼각형으로 기록한 경계만 성공 처리한다.
    if(tid==0 && Remaining==3)
    {
        for(uint i=0;i<count;i++)if(Active(i)!=0)
        {
            Ring.Store3(i*128+64,uint3(i,Nxt(i),Nxt(Nxt(i))));Ring.Store(i*128+76,1);
            Ring.Store(leader*128+84,1);uint ignored;Meta.InterlockedAdd(12,1,ignored);break;
        }
    }
}
