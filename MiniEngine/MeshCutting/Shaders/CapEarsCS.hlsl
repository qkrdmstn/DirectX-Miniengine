#include "CutCommon.hlsli"


// 루프 당 스레드 그룹 크기
#ifndef CAP_THREADS
#define CAP_THREADS 256
#endif

// 루프 캐시 상한은 연결 위치의 11비트 범위로 제한한다.
#ifndef CAP_CACHE
#define CAP_CACHE 1976
#endif
#if CAP_CACHE > 2048
#error CAP_CACHE exceeds the 11-bit link index range
#endif

// groupshared: 같은 스레드 그룹 안의 스레드들만 함께 쓰는 메모리 (공유 메모리)
// 라운드 진행 상태
groupshared uint Remaining, Removed, LastRemoved;
groupshared float Orientation;

// 루프 정보
groupshared uint Cached, Mapped, MapBase, LoopLeader, AllPoints, LoopLength, Rotation;

// 루프 캐시(최대 CAP_CACHE개)
groupshared float2 CachedPoints[CAP_CACHE];
// Link: bit0 활성, bit1~2 종류, bit3 선택, bit4 reflex, bit5~15 Next, bit16~26 Prev.
groupshared uint CachedLink[CAP_CACHE];

// ear 검사 목록
groupshared uint TestList[CAP_CACHE];
groupshared uint TestCount;

// prefix sum 보조
groupshared uint WaveCount, ZeroSlot, ScanTotal, WarpTotal[CAP_THREADS / 4];

// cap mode 설정
cbuffer EarMode : register(b1)
{
    uint SerialMode;
};

// 모든 스레드가 참여하여 경계 순서의 배타적 누적합과 전체 합을 구한다.
uint BlockExclusiveSum(uint value, uint waveSlot, out uint total)
{
    // wave 내부 합을 구하고 마지막 lane이 wave 합계를 기록한다.
    uint lanes = WaveGetLaneCount(), lane = WaveGetLaneIndex(), wave = waveSlot / lanes;
    uint prefix = WavePrefixSum(value);
    uint lastLane = WaveActiveMax(lane);
    if (lane == lastLane)
        WarpTotal[wave] = prefix + value;
    GroupMemoryBarrierWithGroupSync();

    // 한 wave로 처리할 수 없으면 tid 0이 순차 누적한다.
    if (WaveCount > lanes)
    {
        if (waveSlot == ZeroSlot)
        {
            uint sum = 0;
            for (uint w = 0; w < WaveCount; w++)
            {
                uint next = WarpTotal[w];
                WarpTotal[w] = sum;
                sum += next;
            }
            ScanTotal = sum;
        }
    }
    else if (wave == 0)
    {
        uint item = lane < WaveCount ? WarpTotal[lane] : 0;
        uint sum = WavePrefixSum(item);
        uint all = WaveActiveSum(item);
        if (lane == 0)
            ScanTotal = all;
        if (lane < WaveCount)
            WarpTotal[lane] = sum;
    }
    GroupMemoryBarrierWithGroupSync();

    // wave 간 offset과 wave 내부 누적합을 결합한다.
    total = ScanTotal;
    return WarpTotal[wave] + prefix;
}

// 경계 위치를 VRAM 순서 테이블의 원본 Ring 슬롯으로 변환한다.
uint Slot(uint i)
{
    // 캐시와 큰 loop는 순서 테이블을 읽고 나머지는 원래 슬롯을 반환한다.
    return (Cached | Mapped) != 0 ? Ring.Load(HashLinksOffset + MaxSegments * 8 + (MapBase + i) * 4) : i;
}

// 현재 경계 위치의 투영 좌표를 읽는다.
float2 P2(uint i)
{
    // 캐시 위치 또는 Ring 슬롯의 좌표를 반환한다.
    return Cached != 0 ? CachedPoints[i] : Point2(Slot(i));
}

// 연결 word에서 다음 경계 위치를 읽는다.
uint LinkNext(uint word)
{
    // 5~15비트의 Next 필드를 추출한다.
    return (word >> 5) & 0x7FF;
}

// 연결 word에서 이전 경계 위치를 읽는다.
uint LinkPrev(uint word)
{
    // 16~26비트의 Prev 필드를 추출한다.
    return (word >> 16) & 0x7FF;
}

// 다른 필드를 보존하며 다음 경계 위치를 원자적으로 갱신한다.
void SetLinkNext(uint i, uint next)
{
    // Next 필드만 지운 뒤 새 위치를 넣는다.
    InterlockedAnd(CachedLink[i], ~(0x7FFu << 5));
    InterlockedOr(CachedLink[i], next << 5);
}

// 다른 필드를 보존하며 이전 경계 위치를 원자적으로 갱신한다.
void SetLinkPrev(uint i, uint prev)
{
    // Prev 필드만 지운 뒤 새 위치를 넣는다.
    InterlockedAnd(CachedLink[i], ~(0x7FFu << 16));
    InterlockedOr(CachedLink[i], prev << 16);
}

// 현재 경계의 다음 점을 읽는다.
uint Nxt(uint i)
{
    // 캐시 연결 word에서 다음 위치를 추출한다.
    if (Cached != 0)
        return LinkNext(CachedLink[i]);

    // 원래 슬롯의 다음 연결을 현재 경계 위치로 반환한다.
    uint slot = Next(Slot(i));
    return Mapped != 0 ? Ring.Load(slot * 128 + 112) : slot;
}

// 현재 경계의 이전 점을 읽는다.
uint Prv(uint i)
{
    // 캐시 연결 word에서 이전 위치를 추출한다.
    if (Cached != 0)
        return LinkPrev(CachedLink[i]);

    // 원래 슬롯의 이전 연결을 현재 경계 위치로 반환한다.
    uint slot = Prev(Slot(i));
    return Mapped != 0 ? Ring.Load(slot * 128 + 112) : slot;
}

// 현재 loop의 활성 점인지 확인한다.
uint Active(uint i)
{
    // 활성 비트 또는 현재 loop의 슬롯 상태를 반환한다.
    return Cached != 0 ? (CachedLink[i] & 1) : (Ring.Load(Slot(i) * 128 + 48) == LoopLeader ? Ring.Load(Slot(i) * 128 + 80) : 0);
}

// 현재 라운드의 귀/공선 후보를 읽는다.
uint Kind(uint i)
{
    // 후보 종류 두 비트 또는 슬롯의 후보 종류를 반환한다.
    return Cached != 0 ? ((CachedLink[i] >> 1) & 3) : Ring.Load(Slot(i) * 128 + 60);
}

// reflex 검사 또는 확정 제거 마스크를 읽는다.
uint Selected(uint i)
{
    // 선택 비트 또는 슬롯의 선택 상태를 반환한다.
    return Cached != 0 ? ((CachedLink[i] >> 3) & 1) : Ring.Load(Slot(i) * 128 + 88);
}

// 귀 검사용 reflex 플래그를 선택 상태와 분리해 읽는다.
uint Reflex(uint i)
{
    // 작은 캐시는 bit4, 큰 loop는 별도 슬롯 word에 reflex를 보관한다.
    return Cached != 0 ? ((CachedLink[i] >> 4) & 1) : Ring.Load(Slot(i) * 128 + 92);
}

// 경계에 닿는 점도 귀 안에 있다고 판정해 잘못된 대각선을 만들지 않는다.
bool InTriangle(float2 p, float2 a, float2 b, float2 c, float sign)
{
    return Cross2(b - a, p - a) * sign >= -Epsilon * Epsilon && Cross2(c - b, p - b) * sign >= -Epsilon * Epsilon
        && Cross2(a - c, p - c) * sign >= -Epsilon * Epsilon;
}

// 캐시 후보 종류를 읽어 개수를 세고 검사 목록과 경계 순서의 통과 순위로 잔여 정점을 보호한다.
uint SelectCached(uint waveSlot, uint round, uint zeroRounds, out uint marked)
{
    // 연속 k개 위치를 소유하고 미리 계산된 후보 종류를 읽어 개수를 센다.
    uint k = (LoopLength + CAP_THREADS - 1) / CAP_THREADS;
    uint begin = waveSlot * k, end = min(begin + k, LoopLength), candidates = 0;
    for (uint i = begin; i < end; i++) // begin부터 정점 k개 순회
        candidates += Kind(i) != 0 ? 1 : 0;

    // 스레드별 후보 수를 경계 전체 후보 번호로 바꾼다.
    uint count;
    uint start = BlockExclusiveSum(candidates, waveSlot, count);
    marked = 0;

    // 두 번째 무진행 라운드는 같은 경계에서 모든 후보를 다시 검사한다.
    for (uint attempt = 0; attempt < 2; attempt++)
    {
        uint index = start, passes = 0;
        for (uint v = begin; v < end; v++) // 각 스레드마다 k개의 정점을 검사
        {
            uint kind = Kind(v);
            bool mark = false;
            if (kind != 0)
            {
                uint q = round & 1; // 이번 라운드가 짝수면 0, 홀수면 1
                mark = attempt != 0 || ((index & 1) == q && !(q == 0 && (count & 1) != 0 && index == count - 1)); // 후보 정점인지 아닌지 표시
                index++;
            }
            
            // 볼록 후보에 대해 InTriangle 검사
            if (mark)
            {
                uint a = Prv(v), c = Nxt(v);
                if (kind == 1)
                    for (uint t = 0; t < TestCount; t++)
                    {
                        uint test = TestList[t];
                        if (test != a && test != v && test != c                     // test 정점이 삼각 분할 대상이 아니며,
                        && InTriangle(P2(test), P2(a), P2(v), P2(c), Orientation))  // 삼각 분할 내부에 존재하지 않아야 한다.
                        {
                            mark = false;
                            break;
                        }
                    }
            }

            // 삼각 분할 결과 기록
            CachedLink[v] = (CachedLink[v] & ~8u) | (mark ? 8u : 0u);
            passes += mark ? 1 : 0; // 통과한 후보 개수
        }

        // 통과한 ear가 너무 많으면 루프 앞쪽부터 일부만 남긴다. (남은 정점이 3개 미만이면 마지막 삼각형을 만들 수 없기 때문)
        uint total;
        uint rank = BlockExclusiveSum(passes, waveSlot, total); // 통과한 ear에 루프 순서 번호를 매긴다. 
        
        marked += attempt != 0 ? count : count / 2; // 이번 검사 대상으로 표시한 후보 수
        if (total == 0 && zeroRounds != 0 && attempt == 0) // 이번에 통과한 ear가 0개이고, 직전 라운드도 0개라면, attempt1(모든 후보 검사, 하나만 자르기)를 수행
            continue;
        
        // 이번에 자를 최대 개수를 현재 남은 정점의 개수-3로 설정 (최소 3개는 남긴다)
        // attempt가 1이거나 SerialMode라면 하나만 자른다.
        uint limit = attempt != 0 || SerialMode != 0 ? min(1u, Remaining - 3) : Remaining - 3;
        for (uint clear = begin; clear < end; clear++)  // 내 구간의 정점 순회
        {
            if (Selected(clear) != 0)   //
            {
                if (rank >= limit)
                    CachedLink[clear] &= ~8u;
                rank++;
            }
        }
        
        return min(total, limit);
    }
    return 0;
}

// 큰 loop도 후보 종류를 읽어 개수를 세고 canonical 경계 순서의 scan으로 제거 수를 제한한다.
uint SelectMapped(uint waveSlot, uint round, uint zeroRounds, out uint marked)
{
    // 각 스레드가 소유한 연속 경계 위치에서 미리 계산된 후보 종류를 읽어 개수를 센다.
    uint k = (LoopLength + CAP_THREADS - 1) / CAP_THREADS;
    uint begin = waveSlot * k, end = min(begin + k, LoopLength), candidates = 0;
    for (uint i = begin; i < end; i++)
        candidates += Kind(i) != 0 ? 1 : 0;

    // 경계 순서의 후보 순번을 전체 그룹에 배분한다.
    uint count;
    uint start = BlockExclusiveSum(candidates, waveSlot, count);
    marked = 0;

    // 무진행 후속 라운드는 같은 경계를 다시 검사해 최소 한 후보를 남긴다.
    for (uint attempt = 0; attempt < 2; attempt++)
    {
        uint index = start, passes = 0;
        for (uint v = begin; v < end; v++)
        {
            uint kind = Kind(v);
            bool mark = false;
            if (kind != 0)
            {
                uint parity = round & 1;
                mark = attempt != 0 || ((index & 1) == parity && !(parity == 0 && (count & 1) != 0 && index == count - 1));
                index++;
            }
            if (mark)
            {
                uint a = Prv(v), c = Nxt(v);
                if (kind == 1)
                {
                    float2 pointA = P2(a), pointV = P2(v), pointC = P2(c);
                    for (uint test = 0; test < LoopLength; test++)
                    {
                        uint testSlot = Slot(test);
                        if (test != a && test != v && test != c
                            && Ring.Load(testSlot * 128 + 48) == LoopLeader && Ring.Load(testSlot * 128 + 80) != 0
                            && (AllPoints != 0 || Ring.Load(testSlot * 128 + 92) != 0)
                            && InTriangle(Point2(testSlot), pointA, pointV, pointC, Orientation))
                        {
                            mark = false;
                            break;
                        }
                    }
                }
            }
            Ring.Store(Slot(v) * 128 + 88, mark ? 1 : 0);
            passes += mark ? 1 : 0;
        }
        AllMemoryBarrierWithGroupSync();

        // 통과 후보를 scan하고 경계 앞쪽부터 잔여 정점 한도를 적용한다.
        uint total;
        uint rank = BlockExclusiveSum(passes, waveSlot, total);
        marked += attempt != 0 ? count : count / 2;
        if (total == 0 && zeroRounds != 0 && attempt == 0)
            continue;
        uint limit = attempt != 0 || SerialMode != 0 ? min(1u, Remaining - 3) : Remaining - 3;
        for (uint clear = begin; clear < end; clear++)
            if (Selected(clear) != 0)
            {
                if (rank >= limit)
                    Ring.Store(Slot(clear) * 128 + 88, 0);
                rank++;
            }
        return min(total, limit);
    }
    return 0;
}


// 루프마다 스레드 그룹 하나를 할당하고 각 선분을 스레드에 나눈다.
// 검사점 플래그와 후보 종류를 라운드마다 계산하고 귀 검사와 안전한 동시 제거를 수행한다.
[numthreads(CAP_THREADS, 1, 1)]
void main(uint3 group : SV_GroupID, uint tid : SV_GroupIndex)
{
    //유효 loop인지 확인하고, 그룹의 대표 선분 스레드로 그룹 공통 상태를 초기화한다.
    uint leader = group.x + group.y * 65535; // 2차원 그룹 좌표를 1차원 그룹 좌표 idx로 변환
    uint count = min(Meta.Load(4), MaxSegments);
    
    // 슬롯 유효성 확인 || 현재 슬롯이 해당 루프의 대표 선분 슬롯인지 확인 || 유효 
    if (leader >= count || Ring.Load(leader * 128 + 48) != leader || (Ring.Load(leader * 128 + 52) & 5) != 0)
        return;

    
    // lane: 각 wave 내부의 스레드가 차지하는 자리
    if (tid == 0)
        WaveCount = 0;  // 그룹의 대표 스레드가 waveCount를 0으로 초기화
    GroupMemoryBarrierWithGroupSync();
    
    // 각 wave의 처음으로 활성되는 lane이 그룹 내의 wave 번호를 하나 받는다.
    uint waveNumber = 0;                                                    // 각 스레드가 자신만의 number를 생성
    if (WaveIsFirstLane())                                                  // 자신의 wave에서 번호가 가장 작은 활성 lane인지 확인
        InterlockedAdd(WaveCount, 1, waveNumber);                           // 그룹 공유 카운터 1 증가 시키고 waveNumber에도 반영한다.
    waveNumber = WaveReadLaneFirst(waveNumber);                             // 각 wave의 모든 활성 lane은 첫 번째 활성 lane의 waveNumber를 자기 변수에 복사한다.
    uint waveSlot = waveNumber * WaveGetLaneCount() + WaveGetLaneIndex();   // wave 번호화 lane을 합쳐서 그룹 내부의 waveSlot 인덱스를 만든다.
    if (tid == 0)                                                           // 해당 w그룹을 대표하는 스레드의 waveSlot을 공유 변수에 저장
        ZeroSlot = waveSlot;

    // 그룹의 0번 스레드가 leader에서 출발해 루프를 한 바퀴 돌면서 시작 위치, 저장 방식 등을 초기화한다.
    if (tid == 0)
    {
        
        // 순회 전 공유 변수 초기화
        LoopLeader = leader;                                            // 현재 루프의 대표 슬롯 번호 (= 루프 ID)
        AllPoints = (Ring.Load(leader * 128 + 52) & 2) != 0 ? 1 : 0;    // 접촉 위험(bad 2) 루프면 ear 검사 때 모든 점을 검사
        LoopLength = 0;                                                 // 루프 길이, 0 = 닫힌 루프 확인 실패 (순회 성공 시 갱신)
        Rotation = 0;                                                   // 시작 순번, 순회 중 좌표가 가장 작은 점으로 갱신 (매 실행마다 같은 순서, 디버깅용)
        Mapped = 0;                                                     // CAP_CACHE 초과 루프를 Ring에 펼쳐 처리하는지 (순회 후 결정)
        MapBase = 0;                                                    // 캐시와 Mapped용 Ring 전용 구간의 시작 위치 (순회 후 예약)
        
        // 루프 순회
        uint2 minKey = uint2(~0u, ~0u);                                 //
        uint slot = leader;                                             // 현재 위치
        
        // 루프에 속한 각 선분(슬롯) or p번째 선분의 시작점들을 순회한다.    
        for (uint p = 0; p < min(count, 4096u); p++)
        {
            if (slot >= count) // 슬롯 유효성 검사
                break;
            if (Ring.Load(slot * 128 + 48) != leader) // 다른 루프 선분으로 넘어간 경우
                break;
            if (p < CAP_CACHE)
                TestList[p] = slot;
            
            // 순회하면서 Rotation 결정
            uint2 key = asuint(Point2(slot));
            if (key.x < minKey.x || (key.x == minKey.x && key.y < minKey.y))
            {
                minKey = key;
                Rotation = p;
            }
            
            // 다음 슬롯으로 이동
            slot = Next(slot);
            if (slot == leader) // 시작점으로 돌아온 경우 break
            {
                LoopLength = p + 1;
                break;
            }
        }

        // 순회 실패는 그룹 전체가 cap을 생략하도록 전달한다.
        Cached = LoopLength <= CAP_CACHE ? 1 : 0;
        Mapped = LoopLength > CAP_CACHE ? 1 : 0;

        // 순회에 성공한 경계의 최대 길이를 기록하고 모든 loop의 순서 테이블 공간을 예약한다.
        if (LoopLength != 0)
        {
            uint ignored;
            Meta.InterlockedMax((61 + 2 * Sections) * 4, LoopLength, ignored);
            Meta.InterlockedAdd((63 + 2 * Sections) * 4, LoopLength, MapBase);
            if (Mapped != 0)
                Meta.InterlockedAdd((62 + 2 * Sections) * 4, 1, ignored);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    
    // 루프 내의 정점이 3개 미만이면 cap 생성 X
    if (LoopLength < 3)
        return;
    uint size = LoopLength;

    // 루프의 p번째 정점이 Ring의 몇 번 슬롯인지 알려주는 테이블을 구성한다.
    // 이때, Rotation 기준으로 시작하게 구성한다.
    if (Cached != 0)
    {
        // 임시 슬롯 목록을 회전해 VRAM 순서 테이블과 좌표 캐시를 채운다.
        for (uint cache = tid; cache < size; cache += CAP_THREADS)
        {
            uint slot = TestList[(cache + Rotation) % size];
            Ring.Store(HashLinksOffset + MaxSegments * 8 + (MapBase + cache) * 4, slot);
            CachedPoints[cache] = Point2(slot);

            // 활성 비트와 양방향 연결을 초기화한다.
            uint next = (cache + 1) % size, prev = (cache + size - 1) % size;
            CachedLink[cache] = 1 | (next << 5) | (prev << 16);
        }
    }
    else if (Mapped != 0)  // 루프 내 정점이 CAP_CACHE개를 넘어서 group shared 캐시에 들어가지 않는 경우, Ring에 기록한다.
    {
        // loop 전용 scratch 구간에 canonical 순서와 Ring 슬롯 역방향 순위를 기록한다.
        if (tid == 0)
        {
            uint slot = leader;
            for (uint p = 0; p < Rotation; p++)
                slot = Next(slot);
            for (uint p = 0; p < size; p++)
            {
                Ring.Store(HashLinksOffset + MaxSegments * 8 + (MapBase + p) * 4, slot);
                Ring.Store(slot * 128 + 112, p);
                slot = Next(slot);
            }
        }
    }

    // 순서 테이블과 캐시 준비를 끝내고 TestList를 라운드에서 재사용하기 전에 동기화한다.
    AllMemoryBarrierWithGroupSync();

    // 스레드 하나가 루프에 남은 정점 수와 루프의 감김 방향을 계산한다.
    // 남은 점 수와 다각형 면적 → 감김 방향(Orientation). 면적 0이거나 4096 초과면 끝
    if (tid == 0)
    {
        Remaining = 0;      // 현재 라운드에서 루프에 남은 정점 수
        Removed = 0;
        float area = 0;
        for (uint j = 0; j < size; j++) // 남은 정점을 순회하면서 면적을 계산한다.
        {
            if (Active(j) != 0)
            {
                Remaining++;
                area += Cross2(P2(j), P2(Nxt(j)));
            }
        }
        Orientation = area >= 0 ? 1 : -1; // 면적의 부호에 따라 감김 방향을 확인

        // 아주 큰/퇴화한 단면은 surface 절단을 유지하고 cap만 생략한다.
        if (Remaining > 4096 || abs(area) < Epsilon * Epsilon)
            Remaining = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    
    // 남은 정점이 3개 미만이면 return
    if (Remaining < 3)
        return;

    
    // ear clipping 라운드 반복
    uint rounds = 0, markedSum = 0, removedSum = 0, zeroRounds = 0; // 진단용 지역 변수
    uint roundLimit = min(LoopLength, 4096u);                       // 반복 상한
    for (uint round = 0; round < roundLimit && Remaining > 3; round++)  // Remaining이 3개보다 많은 동안 round 상한 내에서 라운드를 반복
    {
        
        // 검사점은 reflex/collinear 정점이다. 검사점 플래그와 별도로 후보 종류도 함께 준비한다.
        uint myTestCount = 0;
        
        // 각 스레드가 맡은 모든 정점의 검사점 플래그와 후보 종류를 계산한다.
        for (uint reflex = tid; reflex < size; reflex += CAP_THREADS)
        {
            // 정점 유효성을 확인하고 비활성 정점의 후보 종류는 0으로 둔다.
            uint active = Active(reflex), kind = 0, flag = 0;
            if (active != 0)
            {
                // 같은 외적으로 볼록/오목과 귀/공선 후보 종류를 판정한다.
                float2 p = P2(Prv(reflex)), q = P2(reflex), r = P2(Nxt(reflex));
                float turn = Cross2(q - p, r - q) * Orientation;
                flag = turn <= 0 ? 1 : 0;
                if (abs(turn) <= Epsilon * (length(q - p) + length(r - q)) && dot(q - p, q - r) <= Epsilon * Epsilon)
                    kind = 2;
                else if (turn > 0)
                    kind = 1;
            }
            
            // 작은 루프는 캐시에 저장하고 선택 비트를 지우되 비활성 정점의 검사점 비트는 보존한다.
            if (Cached != 0)
            {
                CachedLink[reflex] = (CachedLink[reflex] & ~(active != 0 ? 30u : 14u))
                    | (kind << 1) | (active != 0 ? flag << 4 : 0u); // 볼록/(오목 or 일직선) 결과 저장
                if (active != 0)
                    myTestCount += AllPoints != 0 || flag != 0 ? 1 : 0; // TestList Size 계산
            }
            // 큰 루프는 모든 후보 종류를 Ring에 저장하고 활성 정점의 검사점 플래그만 갱신한다.
            else
            {
                Ring.Store(Slot(reflex) * 128 + 60, kind);
                if (active != 0)
                    Ring.Store(Slot(reflex) * 128 + 92, flag);
            }
        }

        // 앞에서 표시한 InTriangle 검사 대상 정점들을 TestList에 저장 
        // Todo (정점의 개수가 많으면,, TestList를 구성하지 않고 그냥.. 전체 순회를 때려버린다.. 최적화 필요)
        if (Cached != 0)
        {
            uint total;
            uint offset = BlockExclusiveSum(myTestCount, waveSlot, total); // PrefixSum을 활용해 자신의 공간을 받는다.
            if (waveSlot == ZeroSlot)
                TestCount = total;
            for (uint test = tid; test < size; test += CAP_THREADS)
                if (Active(test) != 0 && (AllPoints != 0 || (CachedLink[test] & 16) != 0))
                    TestList[offset++] = test;
        }
        AllMemoryBarrierWithGroupSync();

        // 볼록한 정점을 번갈아가며 ear 후보로 선택한다.
        if (Cached != 0)
        {
            uint marked;
            SelectCached(waveSlot, round, zeroRounds, marked);
            markedSum += marked;
        }
        else
        {
            uint marked;
            SelectMapped(waveSlot, round, zeroRounds, marked);
            markedSum += marked;
        }
        AllMemoryBarrierWithGroupSync();

        // 선택된 점의 삼각형을 기록하고 연결을 갱신한다.
        for (uint v2 = tid; v2 < size; v2 += CAP_THREADS)
        {
            if (Active(v2) == 0 || Selected(v2) == 0)
                continue;
            uint a = Prv(v2), c = Nxt(v2);
            if (Kind(v2) == 1)
            {
                Ring.Store3(Slot(v2) * 128 + 64, uint3(Slot(a), Slot(v2), Slot(c)));
                Ring.Store(Slot(v2) * 128 + 76, 1);
            }

            // 이웃 연결과 자기 활성 비트를 원자적으로 갱신해 다른 필드를 보존한다.
            if (Cached != 0)
            {
                SetLinkNext(a, c);
                SetLinkPrev(c, a);
                InterlockedAnd(CachedLink[v2], ~1u);
            }
            else
            {
                Ring.Store(Slot(a) * 128 + 40, Slot(c));
                Ring.Store(Slot(c) * 128 + 44, Slot(a));
                Ring.Store(Slot(v2) * 128 + 80, 0);
            }
            InterlockedAdd(Removed, 1);
        }
        AllMemoryBarrierWithGroupSync();

        // 제거 수를 별도 snapshot에 보관하고 다음 라운드 카운터를 함께 초기화한다.
        if (tid == 0)
        {
            Remaining -= Removed;
            LastRemoved = Removed;
            Removed = 0;
        }
        GroupMemoryBarrierWithGroupSync();

        // 다음 라운드의 카운터 갱신과 경합하지 않는 snapshot으로 진단과 중단을 결정한다.
        rounds++;
        removedSum += LastRemoved;
        zeroRounds = LastRemoved == 0 ? zeroRounds + 1 : 0;
        if (LastRemoved == 0 && zeroRounds >= 2)
            break;
    }

    // cap 생성 끝에서 한 번만 dispatch 공통 진단에 합산한다. 진단 카운터 합산, 마지막 삼각형 기록, 대표 슬롯 +84에 완료 표시
    if (tid == 0)
    {
        uint ignored;
        uint base = (58 + 2 * Sections) * 4;
        Meta.InterlockedAdd(base, rounds, ignored);
        Meta.InterlockedAdd(base + 4, markedSum, ignored);
        Meta.InterlockedAdd(base + 8, removedSum, ignored);
    }

    // 마지막 세 점을 기록하고 loop 완료를 표시한다.
    if (tid == 0 && Remaining == 3)
    {
        // 마지막 세 점도 원래 최소 슬롯을 시작점으로 삼는다.
        uint first = size;
        for (uint i = 0; i < size; i++)
            if (Active(i) != 0)
                if (first == size || Slot(i) < Slot(first))
                    first = i;
        if (first != size)
        {
            Ring.Store3(Slot(first) * 128 + 64, uint3(Slot(first), Slot(Nxt(first)), Slot(Nxt(Nxt(first)))));
            Ring.Store(Slot(first) * 128 + 76, 1);
            Ring.Store(leader * 128 + 84, 1);
            uint ignored;
            Meta.InterlockedAdd(12, 1, ignored);
        }
    }
}
