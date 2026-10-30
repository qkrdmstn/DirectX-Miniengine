#include "CutCommon.hlsli"

#ifndef CAP_THREADS
#define CAP_THREADS 256
#endif
groupshared uint Remaining,Removed,LastRemoved;
groupshared float Orientation;
groupshared uint Cached,Mapped,MapBase,LoopLeader,AllPoints,LoopLength,Rotation;
groupshared float2 CachedPoints[1024];
groupshared uint CachedNext[1024],CachedPrev[1024],CachedFlags[1024],CachedSlot[1024];
groupshared uint WaveCount,ZeroSlot,ScanTotal,WarpTotal[CAP_THREADS/4];
cbuffer EarMode : register(b1) { uint SerialMode; };
// 모든 스레드가 참여하여 경계 순서의 배타적 누적합과 전체 합을 구한다.
uint BlockExclusiveSum(uint value,uint waveSlot,out uint total)
{
    // wave 내부 합을 구하고 마지막 lane이 wave 합계를 기록한다.
    uint lanes=WaveGetLaneCount(),lane=WaveGetLaneIndex(),wave=waveSlot/lanes;
    uint prefix=WavePrefixSum(value);
    uint lastLane=WaveActiveMax(lane);
    if(lane==lastLane)WarpTotal[wave]=prefix+value;
    GroupMemoryBarrierWithGroupSync();

    // 한 wave로 처리할 수 없으면 tid 0이 순차 누적한다.
    if(WaveCount>lanes)
    {
        if(waveSlot==ZeroSlot)
        {
            uint sum=0;
            for(uint w=0;w<WaveCount;w++)
            {
                uint next=WarpTotal[w];
                WarpTotal[w]=sum;
                sum+=next;
            }
            ScanTotal=sum;
        }
    }
    else if(wave==0)
    {
        uint item=lane<WaveCount?WarpTotal[lane]:0;
        uint sum=WavePrefixSum(item);
        uint all=WaveActiveSum(item);
        if(lane==0)ScanTotal=all;
        if(lane<WaveCount)WarpTotal[lane]=sum;
    }
    GroupMemoryBarrierWithGroupSync();

    // wave 간 offset과 wave 내부 누적합을 결합한다.
    total=ScanTotal;
    return WarpTotal[wave]+prefix;
}
// 경계 위치를 캐시 또는 scan map의 원본 Ring 슬롯으로 변환한다.
uint Slot(uint i)
{
    // 캐시, 큰 loop 매핑 또는 원래 슬롯을 반환한다.
    if(Cached!=0)return CachedSlot[i];
    return Mapped!=0?Ring.Load(HashLinksOffset+MaxSegments*8+(MapBase+i)*4):i;
}
// 현재 경계 위치의 투영 좌표를 읽는다.
float2 P2(uint i)
{
    // 캐시 위치 또는 Ring 슬롯의 좌표를 반환한다.
    return Cached!=0?CachedPoints[i]:Point2(Slot(i));
}
// 현재 경계의 다음 점을 읽는다.
uint Nxt(uint i)
{
    // 캐시 위치 또는 원래 슬롯의 연결을 반환한다.
    if(Cached!=0)return CachedNext[i];
    uint slot=Next(Slot(i));
    return Mapped!=0?Ring.Load(slot*128+112):slot;
}
// 현재 경계의 이전 점을 읽는다.
uint Prv(uint i)
{
    // 캐시 위치 또는 원래 슬롯의 연결을 반환한다.
    if(Cached!=0)return CachedPrev[i];
    uint slot=Prev(Slot(i));
    return Mapped!=0?Ring.Load(slot*128+112):slot;
}
// 현재 loop의 활성 점인지 확인한다.
uint Active(uint i)
{
    // 활성 비트 또는 현재 loop의 슬롯 상태를 반환한다.
    return Cached!=0?(CachedFlags[i]&1):(Ring.Load(Slot(i)*128+48)==LoopLeader?Ring.Load(Slot(i)*128+80):0);
}
// 현재 라운드의 귀/공선 후보를 읽는다.
uint Kind(uint i)
{
    // 후보 종류 두 비트 또는 슬롯의 후보 종류를 반환한다.
    return Cached!=0?((CachedFlags[i]>>1)&3):Ring.Load(Slot(i)*128+60);
}
// reflex 검사 또는 확정 제거 마스크를 읽는다.
uint Selected(uint i)
{
    // 선택 비트 또는 슬롯의 선택 상태를 반환한다.
    return Cached!=0?((CachedFlags[i]>>3)&1):Ring.Load(Slot(i)*128+88);
}
// 귀 검사용 reflex 플래그를 선택 상태와 분리해 읽는다.
uint Reflex(uint i)
{
    // 작은 캐시는 bit4, 큰 loop는 별도 슬롯 word에 reflex를 보관한다.
    return Cached!=0?((CachedFlags[i]>>4)&1):Ring.Load(Slot(i)*128+92);
}
// 경계에 닿는 점도 귀 안에 있다고 판정해 잘못된 대각선을 만들지 않는다.
bool InTriangle(float2 p,float2 a,float2 b,float2 c,float sign)
{
    return Cross2(b-a,p-a)*sign>=-Epsilon*Epsilon && Cross2(c-b,p-b)*sign>=-Epsilon*Epsilon
        && Cross2(a-c,p-c)*sign>=-Epsilon*Epsilon;
}
// 캐시 후보를 경계 순서로 교대 표시하고 통과 순위로 잔여 정점을 보호한다.
uint SelectCached(uint waveSlot,uint round,uint zeroRounds,out uint marked)
{
    // 연속 k개 위치를 소유하고 내부 점 검사 없이 후보 종류만 먼저 구한다.
    uint k=(LoopLength+CAP_THREADS-1)/CAP_THREADS;
    uint begin=waveSlot*k,end=min(begin+k,LoopLength),candidates=0;
    for(uint i=begin;i<end;i++)
    {
        uint kind=0;
        if(Active(i)!=0)
        {
            float2 p=P2(Prv(i)),q=P2(i),r=P2(Nxt(i));
            float turn=Cross2(q-p,r-q)*Orientation;
            if(abs(turn)<=Epsilon*(length(q-p)+length(r-q)) && dot(q-p,q-r)<=Epsilon*Epsilon)kind=2;
            else if(turn>0)kind=1;
        }
        CachedFlags[i]=(CachedFlags[i]&~14u)|(kind<<1);
        candidates+=kind!=0?1:0;
    }

    // 스레드별 후보 수를 경계 전체 후보 번호로 바꾼다.
    uint count;
    uint start=BlockExclusiveSum(candidates,waveSlot,count);
    marked=0;

    // 두 번째 무진행 라운드는 같은 경계에서 모든 후보를 다시 검사한다.
    for(uint attempt=0;attempt<2;attempt++)
    {
        uint index=start,passes=0;
        for(uint v=begin;v<end;v++)
        {
            uint kind=Kind(v);
            bool mark=false;
            if(kind!=0)
            {
                uint q=round&1;
                mark=attempt!=0 || ((index&1)==q && !(q==0 && (count&1)!=0 && index==count-1));
                index++;
            }
            if(mark)
            {
                uint a=Prv(v),c=Nxt(v);
                if(kind==1)for(uint test=0;test<LoopLength;test++)
                    if(test!=a && test!=v && test!=c && Active(test)!=0
                        && (AllPoints!=0 || (CachedFlags[test]&16)!=0)
                        && InTriangle(P2(test),P2(a),P2(v),P2(c),Orientation))
                    {
                        mark=false;
                        break;
                    }
            }
            CachedFlags[v]=(CachedFlags[v]&~8u)|(mark?8u:0u);
            passes+=mark?1:0;
        }

        // 통과 후보를 다시 scan하여 경계 앞쪽부터 최대 Remaining-3개만 남긴다.
        uint total;
        uint rank=BlockExclusiveSum(passes,waveSlot,total);
        marked+=attempt!=0?count:count/2;
        if(total==0 && zeroRounds!=0 && attempt==0)continue;
        uint limit=attempt!=0 || SerialMode!=0?min(1u,Remaining-3):Remaining-3;
        for(uint clear=begin;clear<end;clear++)if(Selected(clear)!=0)
        {
            if(rank>=limit)CachedFlags[clear]&=~8u;
            rank++;
        }
        return min(total,limit);
    }
    return 0;
}
// 큰 loop도 canonical 경계 순서의 scan으로 후보를 표시하고 제거 수를 제한한다.
uint SelectMapped(uint waveSlot,uint round,uint zeroRounds,out uint marked)
{
    // 각 스레드가 소유한 연속 경계 위치에서 귀와 공선 후보를 판정한다.
    uint k=(LoopLength+CAP_THREADS-1)/CAP_THREADS;
    uint begin=waveSlot*k,end=min(begin+k,LoopLength),candidates=0;
    for(uint i=begin;i<end;i++)
    {
        uint kind=0;
        if(Active(i)!=0)
        {
            float2 p=P2(Prv(i)),q=P2(i),r=P2(Nxt(i));
            float turn=Cross2(q-p,r-q)*Orientation;
            if(abs(turn)<=Epsilon*(length(q-p)+length(r-q)) && dot(q-p,q-r)<=Epsilon*Epsilon)kind=2;
            else if(turn>0)kind=1;
        }
        Ring.Store(Slot(i)*128+60,kind);
        candidates+=kind!=0?1:0;
    }
    AllMemoryBarrierWithGroupSync();

    // 경계 순서의 후보 순번을 전체 그룹에 배분한다.
    uint count;
    uint start=BlockExclusiveSum(candidates,waveSlot,count);
    marked=0;

    // 무진행 후속 라운드는 같은 경계를 다시 검사해 최소 한 후보를 남긴다.
    for(uint attempt=0;attempt<2;attempt++)
    {
        uint index=start,passes=0;
        for(uint v=begin;v<end;v++)
        {
            uint kind=Kind(v);
            bool mark=false;
            if(kind!=0)
            {
                uint parity=round&1;
                mark=attempt!=0 || ((index&1)==parity && !(parity==0 && (count&1)!=0 && index==count-1));
                index++;
            }
            if(mark)
            {
                uint a=Prv(v),c=Nxt(v);
                if(kind==1)
                {
                    float2 pointA=P2(a),pointV=P2(v),pointC=P2(c);
                    for(uint test=0;test<LoopLength;test++)
                    {
                        uint testSlot=Slot(test);
                        if(test!=a && test!=v && test!=c
                            && Ring.Load(testSlot*128+48)==LoopLeader && Ring.Load(testSlot*128+80)!=0
                            && (AllPoints!=0 || Ring.Load(testSlot*128+92)!=0)
                            && InTriangle(Point2(testSlot),pointA,pointV,pointC,Orientation))
                        {
                            mark=false;
                            break;
                        }
                    }
                }
            }
            Ring.Store(Slot(v)*128+88,mark?1:0);
            passes+=mark?1:0;
        }
        AllMemoryBarrierWithGroupSync();

        // 통과 후보를 scan하고 경계 앞쪽부터 잔여 정점 한도를 적용한다.
        uint total;
        uint rank=BlockExclusiveSum(passes,waveSlot,total);
        marked+=attempt!=0?count:count/2;
        if(total==0 && zeroRounds!=0 && attempt==0)continue;
        uint limit=attempt!=0 || SerialMode!=0?min(1u,Remaining-3):Remaining-3;
        for(uint clear=begin;clear<end;clear++)if(Selected(clear)!=0)
        {
            if(rank>=limit)Ring.Store(Slot(clear)*128+88,0);
            rank++;
        }
        return min(total,limit);
    }
    return 0;
}
// 캐시 시작점을 위치 키로 고정하고 그룹 내부에서 귀 검사와 안전한 다중 제거를 병렬 수행한다.
[numthreads(CAP_THREADS,1,1)]
void main(uint3 group:SV_GroupID,uint tid:SV_GroupIndex)
{
    // 유효 loop를 확인하고 그룹 공통 상태를 초기화한다.
    uint leader=group.x+group.y*65535,count=min(Meta.Load(4),MaxSegments);
    if(leader>=count||Ring.Load(leader*128+48)!=leader||(Ring.Load(leader*128+52)&5)!=0)return;

    // 실행 wave에 번호를 부여하므로 tid와 lane 순서의 대응을 가정하지 않는다.
    if(tid==0)WaveCount=0;
    GroupMemoryBarrierWithGroupSync();
    uint waveNumber=0;
    if(WaveIsFirstLane())InterlockedAdd(WaveCount,1,waveNumber);
    waveNumber=WaveReadLaneFirst(waveNumber);
    uint waveSlot=waveNumber*WaveGetLaneCount()+WaveGetLaneIndex();
    if(tid==0)ZeroSlot=waveSlot;

    // loop 순회와 캐시 적재는 기존 leader 기준으로 초기화한다.
    if(tid==0)
    {
        // leader부터 닫힌 경계를 검증하고 처음 1024개 위치의 슬롯을 보관한다.
        LoopLeader=leader;
        AllPoints=(Ring.Load(leader*128+52)&2)!=0?1:0;
        LoopLength=0;
        Rotation=0;
        Mapped=0;
        MapBase=0;
        uint2 minKey=uint2(~0u,~0u);
        uint slot=leader;
        for(uint p=0;p<min(count,4096u);p++)
        {
            if(slot>=count)break;
            if(Ring.Load(slot*128+48)!=leader)break;
            if(p<1024)CachedSlot[p]=slot;
            uint2 key=asuint(Point2(slot));
            if(key.x<minKey.x || (key.x==minKey.x && key.y<minKey.y))
            {
                minKey=key;
                Rotation=p;
            }
            slot=Next(slot);
            if(slot==leader)
            {
                LoopLength=p+1;
                break;
            }
        }

        // 순회 실패는 그룹 전체가 cap을 생략하도록 전달한다.
        Cached=LoopLength<=1024?1:0;
        Mapped=LoopLength>1024?1:0;

        // 순회에 성공한 경계의 최대 길이를 기록하고 큰 loop 전용 scan 공간을 예약한다.
        if(LoopLength!=0)
        {
            uint ignored;
            Meta.InterlockedMax((61+2*Sections)*4,LoopLength,ignored);
            if(Mapped!=0)
            {
                Meta.InterlockedAdd((62+2*Sections)*4,1,ignored);
                Meta.InterlockedAdd((63+2*Sections)*4,LoopLength,MapBase);
            }
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if(LoopLength<3)return;
    uint size=LoopLength;

    // 연결 초기화 전의 Next 배열을 임시 저장소로 써 회전 원본의 덮어쓰기를 막는다.
    if(Cached!=0)
    {
        for(uint cache=tid;cache<size;cache+=CAP_THREADS)
            CachedNext[cache]=CachedSlot[(cache+Rotation)%size];
        GroupMemoryBarrierWithGroupSync();
    }
    else if(Mapped!=0)
    {
        // loop 전용 scratch 구간에 canonical 순서와 Ring 슬롯 역방향 순위를 기록한다.
        if(tid==0)
        {
            uint slot=leader;
            for(uint p=0;p<Rotation;p++)slot=Next(slot);
            for(uint p=0;p<size;p++)
            {
                Ring.Store(HashLinksOffset+MaxSegments*8+(MapBase+p)*4,slot);
                Ring.Store(slot*128+112,p);
                slot=Next(slot);
            }
        }
        AllMemoryBarrierWithGroupSync();
    }

    // 회전된 슬롯을 읽은 뒤 각 위치의 좌표와 연결 및 압축 플래그를 초기화한다.
    if(Cached!=0)for(uint cache=tid;cache<size;cache+=CAP_THREADS)
    {
        uint src=CachedNext[cache];
        CachedSlot[cache]=src;
        CachedPoints[cache]=Point2(src);
        CachedNext[cache]=(cache+1)%size;
        CachedPrev[cache]=(cache+size-1)%size;
        CachedFlags[cache]=1;
    }
    GroupMemoryBarrierWithGroupSync();

    // 활성 점 수와 방향을 구한다.
    if(tid==0)
    {
        Remaining=0;
        Removed=0;
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

    // 모든 크기의 loop에 같은 교대 표시와 무진행 보호를 적용한다.
    uint rounds=0,markedSum=0,removedSum=0,zeroRounds=0;
    uint roundLimit=min(LoopLength,4096u);
    for(uint round=0;round<roundLimit && Remaining>3;round++)
    {
        // 검사점은 reflex/collinear 정점이다. 캐시는 bit4, 큰 loop는 별도 word에 보관한다.
        for(uint reflex=tid;reflex<size;reflex+=CAP_THREADS)
        {
            if(Active(reflex)==0)continue;
            float turn=Cross2(P2(reflex)-P2(Prv(reflex)),P2(Nxt(reflex))-P2(reflex))*Orientation;
            uint flag=turn<=0?1:0;
            if(Cached!=0)CachedFlags[reflex]=(CachedFlags[reflex]&~16u)|(flag<<4);
            else Ring.Store(Slot(reflex)*128+92,flag);
        }
        AllMemoryBarrierWithGroupSync();

        // 작은 캐시와 큰 loop 매핑 모두 같은 경계 순서 scan 선택을 사용한다.
        if(Cached!=0)
        {
            uint marked;
            SelectCached(waveSlot,round,zeroRounds,marked);
            markedSum+=marked;
        }
        else
        {
            uint marked;
            SelectMapped(waveSlot,round,zeroRounds,marked);
            markedSum+=marked;
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
                Ring.Store(Slot(a)*128+40,Slot(c));
                Ring.Store(Slot(c)*128+44,Slot(a));
                Ring.Store(Slot(v2)*128+80,0);
            }
            InterlockedAdd(Removed,1);
        }
        AllMemoryBarrierWithGroupSync();

        // 제거 수를 별도 snapshot에 보관하고 다음 라운드 카운터를 함께 초기화한다.
        if(tid==0)
        {
            Remaining-=Removed;
            LastRemoved=Removed;
            Removed=0;
        }
        GroupMemoryBarrierWithGroupSync();

        // 다음 라운드의 카운터 갱신과 경합하지 않는 snapshot으로 진단과 중단을 결정한다.
        rounds++;
        removedSum+=LastRemoved;
        zeroRounds=LastRemoved==0?zeroRounds+1:0;
        if(LastRemoved==0 && zeroRounds>=2)break;
    }

    // loop 끝에서 한 번만 dispatch 공통 진단에 합산한다.
    if(tid==0)
    {
        uint ignored;
        uint base=(58+2*Sections)*4;
        Meta.InterlockedAdd(base,rounds,ignored);
        Meta.InterlockedAdd(base+4,markedSum,ignored);
        Meta.InterlockedAdd(base+8,removedSum,ignored);
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
