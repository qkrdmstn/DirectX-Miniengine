#include "CutCommon.hlsli"

// 주변 9셀과 실제 거리로 끝점의 유일한 상대를 찾는다. 해시 충돌은 좌표 검사로 거른다.

// 현재 끝점과 같은 위치에 있는 다른 선분의 끝점이 한개 있는지 확인
uint Match(uint endpoint, uint segmentCount)
{
    // 끝점 좌표 p와 칸을 셀 좌표를 계산
    float2 p = asfloat(Ring.Load2((endpoint / 2) * 128 + 24 + (endpoint & 1) * 8));
    int2 cell = int2(floor(p / Epsilon));
    
    // 주변 3x3 셀 순회
    uint match = 0xffffffff, hits = 0;
    [unroll]
    for (int y = -1; y <= 1; y++)
    {
        [unroll]
        for (int x = -1; x <= 1; x++)
        {
            // 주변 셀 정보 캐싱
            int2 neighbor = cell + int2(x, y);
            uint hash = (asuint(neighbor.x) * 73856093u ^ asuint(neighbor.y) * 19349663u) & HashMask;
            uint head = Ring.Load(HashOffset + hash * 4), steps = 0;
            
            // 해당 셀에 있는 끝점 순회
            while (head != 0 && steps++ < segmentCount * 2)
            {
                uint other = head - 1;                          // 후보 끝점 번호
                head = Ring.Load(HashLinksOffset + other * 4);  // 다음 후보
                if (other / 2 == endpoint / 2)                  // 같은 선분이면 제외
                    continue;
                
                // 현재 셀에 실제로 속한 정점이고, 거리거 Epsilon 이하면 매칭
                float2 q = asfloat(Ring.Load2((other / 2) * 128 + 24 + (other & 1) * 8)); // 이웃 정점의 2d 좌표 q
                if (all(int2(floor(q / Epsilon)) == neighbor) && dot(p - q, p - q) <= Epsilon * Epsilon)
                {
                    match = other;
                    hits++;
                }
            }
        }
    }

    // 매칭 성공 시, 해당 정점 번호를 반환
    return hits == 1 ? match : 0xffffffff;
}

// 선분 i가 다른 선분과 엇갈리거나 닿는지 검사
uint Crossing(uint i, uint count)
{
    // 선분 끝점 a -> b
    float2 a = Point2(i), b = asfloat(Ring.Load2(i * 128 + 32));
    uint risk = 0;
    
    // 모든 선분 순회
    for (uint j = 0; j < count; j++)
    {
        // 자기 자신, 루프에 해당하지 않는 선분 제외
        if (i == j || Ring.Load(j * 128 + 108) == 0)
            continue;
        
        // 선분 j의 끝점 c -> d
        float2 c = Point2(j), d = asfloat(Ring.Load2(j * 128 + 32));
        
        // 바운딩 박스 검사로 컬링    
        if (any(max(a, b) < min(c, d) - Epsilon) || any(max(c, d) < min(a, b) - Epsilon))
            continue;
        
        // 끝점을 공유하는 연결 선분(루프) continue
        if (min(min(length(a - c), length(a - d)), min(length(b - c), length(b - d))) <= Epsilon)
            continue;
        
        // 각 끝점이 다른 선분의 어느 쪽에 위치하는지 외적으로 계산
        float x = Cross2(b - a, c - a), y = Cross2(b - a, d - a), z = Cross2(d - c, a - c), w = Cross2(d - c, b - c);
        
        // 선분 a->b와 정점 c를 외적하면 결과의 크기는 a->b 선분까지의 길이 * 선분의 길이가 된다.
        // 선분ab와 정점 c의 거리가 Epsilon보다 작은지 확인하기 위해 Epsilon에 선분 길이를 곱해서 비교한다.
        float eps = Epsilon * max(length(b - a), length(d - c));
        
        // 각 정점이 상대 선분의 반대 쪽에 위치하는지 확인
        bool separated = (x > eps && y > eps) || (x < -eps && y < -eps) || (z > eps && w > eps) || (z < -eps && w < -eps);
        if (separated)
            continue;
        
        // 두 선분을 서로 교차 검사
        bool proper = ((x > eps && y < -eps) || (x < -eps && y > eps)) && ((z > eps && w < -eps) || (z < -eps && w > eps));
        if (proper)
            return 4;
        
        // 거리가 epslion보다는 가깝지만 교차는 아닌 상황
        risk |= 2;
    }
    return risk;
}

// 활성 선분만 연결하고 접촉 후보는 고립시켜 경계 전파에서 제외한다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 스레드 = 선분
    uint i = id.x;
    uint segmentCount = min(Meta.Load(4), MaxSegments);
    if (i >= segmentCount)
        return;
    
    // 연결하지 않을 선분
    if (segmentCount > 4096 || Ring.Load(i * 128 + 108) == 0) // active[i] == 0
    {
        Ring.Store(i * 128 + 40, i);
        Ring.Store(i * 128 + 44, i);
        Ring.Store(i * 128 + 48, i);
        Ring.Store(i * 128 + 52, 1);
        Ring.Store(i * 128 + 56, i);
        return;
    }
    
    // 선분 i의 양끝점(i*2[시작점], i*2+1[끝점])과 매칭되는 점 a, b 찾기
    uint a = Match(i * 2, segmentCount), b = Match(i * 2 + 1, segmentCount);
    
    // a의 짝이 없거나 || b의 짝이 없거나 || 시작점의 짝 a가 시작점이거나 || 끝점의 짝 b가 끝점이면 bad
    uint bad = (a == 0xffffffff || b == 0xffffffff || (a & 1) != 1 || (b & 1) != 0) ? 1 : 0;
    bad |= Crossing(i, segmentCount); // 선분 i가 접촉 위험: 2, 다른 선분과 교차: 4
    
    // 연결 정보 저장
    uint next = b == 0xffffffff ? i : b / 2;
    uint prev = a == 0xffffffff ? i : a / 2;
    Ring.Store(i * 128 + 40, next);
    Ring.Store(i * 128 + 44, prev);
    Ring.Store(i * 128 + 48, i);
    Ring.Store(i * 128 + 52, bad);
    Ring.Store(i * 128 + 56, next);
    Ring.Store(i * 128 + 80, 1);
}
