#include "CutCommon.hlsli"

// 교차 선분들의 끝점을 연결하기 위한 해시 테이블을 생성하는 쉐이더


// 같은 쉐이더를 4번의 단계로 나눠서 수행하기 때문에 phase를 표시한다.
cbuffer BoundaryPass : register(b1)
{
    uint Phase;
};

// 모서리 중점과 경계 끝점을 해시 셀로 바꾼다.
int2 Cell(float2 p)
{
    return int2(floor(p / Epsilon));
}

// 실제 좌표 검사와 함께 사용할 해시를 계산한다.
uint Hash(int2 cell)
{
    return (asuint(cell.x) * 73856093u ^ asuint(cell.y) * 19349663u) & HashMask;
}

// 두 edge가 같은지 방향과 무관하게 검사
bool SameEdge(float2 a, float2 b, float2 c, float2 d)
{
    return (length(a - c) <= Epsilon && length(b - d) <= Epsilon) || (length(a - d) <= Epsilon && length(b - c) <= Epsilon);
}

// 선분 i를 닫힌 경계 루프에 넣을지 말지 결정한다.
bool Resolve(uint i, uint segmentCount)
{
    // 선분 종류 검사
    uint kind = Ring.Load(i * 128 + 92);
    if (kind == 1) // 삼각형을 가로지르는 선분 -> 유지
        return true;
    
    // 평면 위에 존재하는 선분이면 두 삼각형이 해당 선분을 공유한다. (kind = 2(+), 4(-))
    // 따라서 중복 방지를 위해 kind 4는 제외한다.
    if (kind != 2) 
        return false;
    
    // kind = 2의 선분(선분이 평면 위에 존재)이 루프에 포함되기 위해선 이 선분을 공유하는 삼각형 2개가 평면을 기준으로 +와 -쪽에 하나씩 분포해야 한다.
    // kind = 2와 kind = 4인 같은 선분이 동시에 존재하면 위를 보장할 수 있다.
   
    // 현재 선분의 중점이 위치한 셀 계산
    float2 a = Point2(i), b = asfloat(Ring.Load2(i * 128 + 32)); // a: 시작점, b: 끝점
    int2 cell = Cell((a + b) * 0.5);
    
    // 주변 3x3 셀 탐색
    uint positives = 0, negatives = 0;
    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            int2 neighbor = cell + int2(x, y); // 이웃 셀 인덱스
            uint head = Ring.Load(HashOffset + Hash(neighbor) * 4); // 이웃 칸의 해시 셀
            
            // 이웃 셀에 들어있는 후보 선분들을 탐색, head == 0 이면 해당 셀에 선분이 존재 X
            uint steps = 0;
            while (head != 0 && steps++ < segmentCount)
            {
                uint other = head - 1; // 실제 선분 번호
                head = Ring.Load(HashLinksOffset + other * 4); // 같은 셀에 있는 다음 선분 번호
                float2 c = Point2(other), d = asfloat(Ring.Load2(other * 128 + 32)); // 이웃 셀에 포함된 선분의 시작점과 끝점
                
                // 선택한 이웃 선분이 실제로 그 셀에 있는지(해시 충돌 방지)
                // 현재 선분과 짝을 이루는 이웃 선분인지 확인 (kind = 4에서의 같은 선분을 탐색)
                if (all(Cell((c + d) * 0.5) == neighbor) && SameEdge(a, b, c, d))
                {
                    uint side = Ring.Load(other * 128 + 92);
                    positives += side == 2 ? 1u : 0u;
                    negatives += side == 4 ? 1u : 0u;
                }
            }
        }
    }
    
    // 하나씩 존재하면 루프에 포함한다.
    return positives == 1 && negatives == 1;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint segmentCount = min(Meta.Load(4), MaxSegments); // 실제 선분 수
    
    // 평면 위에 존재하는 선분을 루프 후보로 해시에 넣는다 (스레드 = 선분)
    if (Phase == 0)
    {
        // 선분이 아니거나 일반 선분이면 return
        uint segmentId = id.x;
        if (segmentId >= segmentCount || Ring.Load(segmentId * 128 + 92) == 1)
            return;
        
        // 중점 계산
        float2 midpoint = (Point2(segmentId) + asfloat(Ring.Load2(segmentId * 128 + 32))) * 0.5;
        if (any(abs(floor(midpoint / Epsilon)) > 2e9))
        {
            Fail(8);
            return;
        }
        
        //버킷에 현재 선분 번호를 넣고, 원래 있던 선분을 내 뒤에 링크로 연결
        uint previous;
        Ring.InterlockedExchange(HashOffset + Hash(Cell(midpoint)) * 4, segmentId + 1, previous);
        Ring.Store(HashLinksOffset + segmentId * 4, previous);
    }
    
    // 각 선분을 루프에 남길지 탐색 + 해당 선분이 절단 평면 안에 있는지 확인 (스레드 = 선분)
    else if (Phase == 1)
    {
        // 선분 유효성 검사
        uint segmentId = id.x;
        if (segmentId >= segmentCount)
            return;
        
        // 현재 선분을 유지할지 말지 결정하고 결과 기록
        bool active = Resolve(segmentId, segmentCount);
        Ring.Store(segmentId * 128 + 108, active ? 1u : 0u);
        
        // 유지할 선분이면 절단 평면 안에 들어오는지 확인
        if (active)
        {
            float3 a = asfloat(Ring.Load3(segmentId * 128)), b = asfloat(Ring.Load3(segmentId * 128 + 12)); // 끝점 a, b
            float3 r0 = mul(LocalToRectangle, float4(a, 1)).xyz, r1 = mul(LocalToRectangle, float4(b, 1)).xyz; // 각 점을 절단평면의 좌표계로 변환
            if (any(abs(r0.xz) > float2(HalfWidth, HalfHeight) + Epsilon) || any(abs(r1.xz) > float2(HalfWidth, HalfHeight) + Epsilon)) // 선분이 평면 밖으로 나감 -> 현재 cut job 거절
                Fail(1);
            
            // 루프에 들어갈 선분이 존재함을 표시
            uint ignored;
            Meta.InterlockedOr(20, 4, ignored);
        }
    }
    
    // 버킷 비우기 (스레드 = 버킷)
    else if (Phase == 2)
    {
        uint bucketId = id.x;
        if (bucketId <= HashMask)
            Ring.Store(HashOffset + bucketId * 4, 0);
    }
    
    // 루프 선분의 양 끝점을 해시에 넣기 (스레드 = 끝점)
    else
    {
        uint endpoint = id.x;
        if (endpoint >= segmentCount * 2)
            return;
        
        // 선분 0 -> 끝점 0, 1 
        // 선분 1 -> 끝점 2, 3 ..
        uint segment = endpoint / 2;
        uint isEndVertex = endpoint & 1; // 마지막 비트가 1이면 끝점
        if (Ring.Load(segment * 128 + 108) == 0) //루프에 안들어가는 선분 제외
            return;
        
        // 2D 끝점을 해시 테이블에 삽입
        float2 p = asfloat(Ring.Load2(segment * 128 + 24 + isEndVertex * 8)); // 끝점 좌표를 선분 슬롯에서 읽기
        float2 cellFloat = floor(p / Epsilon); // 칸 번호 계산
        if (any(abs(cellFloat) > 2e9)) // 범위 검사
        {
            Fail(8);
            return;
        }
        
        // 해시 계산
        int2 cell = int2(cellFloat); // int로 변환
        uint hash = (asuint(cell.x) * 73856093u ^ asuint(cell.y) * 19349663u) & HashMask;
        
        // 버킷에 삽입 해당 셀에 이미 정점이 존재한다면(없어도 0을) 링크로 연결
        uint previous;
        Ring.InterlockedExchange(HashOffset + hash * 4, endpoint + 1, previous); // 버킷에 삽입은 원자적으로 수행
        Ring.Store(HashLinksOffset + endpoint * 4, previous); // 링크 연결
    }
}
