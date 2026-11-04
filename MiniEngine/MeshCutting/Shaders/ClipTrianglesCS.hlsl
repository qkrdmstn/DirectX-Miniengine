#include "CutCommon.hlsli"

// 유효한 입력 표면의 양쪽 존재를 기록하고 각 삼각형과 단면 선분을 출력한다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 스레드 번호 = 삼각형 인덱스
    uint tri = id.x;
    if (tri >= Triangles) // 남는 스레드 종료
        return;
    
    // CutSection 목록에서 내 삼각형이 속한 칸찾기
    uint section = 0;
    while (section + 1 < Sections - 1 && tri >= Inputs[section + 1].FirstTriangle)
        section++;
    Section s = Inputs[section];
    uint t = tri - s.FirstTriangle;
    
    // 삼각형에 속한 정점 3개 읽기
    uint3 indices = uint3(Index(s, t * 3), Index(s, t * 3 + 1), Index(s, t * 3 + 2));
    if (any(indices >= s.VertexCount))
    {
        Fail(4);
        return;
    }
    Vertex v[3] = { ReadVertex(s, indices.x), ReadVertex(s, indices.y), ReadVertex(s, indices.z) };
    [unroll]
    for (uint vertex = 0; vertex < 3; vertex++)
        v[vertex].TriangleId = t; // 원본 삼각형 번호 저장
    
    // 두 변의 외적을 통해 넓이 계산
    float3 area = cross(v[1].P - v[0].P, v[2].P - v[0].P);
    if (dot(area, area) <= 1e-24)
        return;
    
    // CPU가 이 Section이 전부 한쪽에 있다고 지정했으면, 자르지 않고 특정 방향에 그대로 출력
    uint ignored;
    if (s.Reserved != 0)
    {
        Meta.InterlockedOr(20, 1u << (s.Reserved - 1), ignored);
        EmitTriangle(s.Reserved - 1, section, v[0], v[1], v[2]);
        return;
    }
    
    // 각 정점의 부호 계산
    float d[3];
    bool positive = false, negative = false;
    [unroll]
    for (uint j = 0; j < 3; j++)
    {
        d[j] = dot(Plane, float4(v[j].P, 1));
        if (abs(d[j]) <= Epsilon)
            d[j] = 0;
        positive |= d[j] > 0;
        negative |= d[j] < 0;
    }
    
    // 세 점 모두 평면 위에 존재하면, 출력 버퍼 A에 저장
    if (!positive && !negative)
    {
        EmitTriangle(0, section, v[0], v[1], v[2]);
        return;
    }
    
    // A, B side 나누기 0: A, 1: B
    [unroll]
    for (uint side = 0; side < 2; side++)
    {
        Vertex poly[4]; // 한 쪽의 정점 정보를 저장
        uint n = 0;
        [unroll]
        for (uint k = 0; k < 3; k++)
        {
            uint p = (k + 2) % 3; // 세 정점 중 2개의 정점 인덱스 k, p를 선택
            float a = side == 0 ? d[p] : -d[p]; // 현재 검사하는 side의 부호를 양수로
            float b = side == 0 ? d[k] : -d[k];
            
            // 두 거리 값의 부호가 다르면 교차
            if ((a > 0 && b < 0) || (a < 0 && b > 0))
                poly[n++] = IntersectEdge(v[p], v[k], a, b);
            if (b >= 0)
                poly[n++] = v[k];
        }
        
        for (uint q = 1; q + 1 < n; q++)
        {
            // 넓이 0인 삼각형을 제외하고, 자른 조각에 대해 poly[0]을 꼭짓점으로 하는 삼각분할을 수행한다. 
            if (dot(cross(poly[q].P - poly[0].P, poly[q + 1].P - poly[0].P), cross(poly[q].P - poly[0].P, poly[q + 1].P - poly[0].P)) > 1e-24)
            {
                EmitTriangle(side, section, poly[0], poly[q], poly[q + 1]);
                
                // cut 작업 flag를 표시한다. -> AABB로 Cut Job을 생성하기 때문에 실제로 잘린 메시임을 표시해서 오작동(빈 메시 컴포넌트 추가 등)을 방지
                if (side == 0 ? positive : negative)
                    Meta.InterlockedOr(20, 1u << side, ignored);
            }
        }
    }
    
    // 삼각형-평면 교차점의 선분을 계산
    if (!positive && !negative) // 삼각형이 평면 위에 있으면 선분 존재 X
        return;
    
    float3 enter = 0, exit = 0; // 교차점 두 개
    bool gotEnter = false, gotExit = false;
    uint kind = 1;
    
    [unroll]
    for (uint e = 0; e < 3; e++)
    {
        // edge 선택 e, n
        uint n = (e + 1) % 3;
        
        // 현재 edge가 평면을 지나가는지 확인하여 교차점 2개를 계산
        if (d[e] <= 0 && d[n] > 0)
        {
            enter = IntersectEdge(v[e], v[n], d[e], d[n]).P;
            gotEnter = true;
        }
        if (d[e] > 0 && d[n] <= 0)
        {
            exit = IntersectEdge(v[e], v[n], d[e], d[n]).P;
            gotExit = true;
        }
    }
    
    // edge의 두 점이 평면 위에 있는 경우
    if (!positive || !negative)
    {
        kind = positive ? 2u : 4u;
        [unroll]
        for (uint edge = 0; edge < 3; edge++)
            if (d[edge] == 0 && d[(edge + 1) % 3] == 0)
            {
                enter = v[(edge + 1) % 3].P;
                exit = v[edge].P;
                gotEnter = gotExit = true;
            }
    }
    
    // 유효하지 않은 선분 return
    if (!gotEnter || !gotExit || length(Project(enter) - Project(exit)) <= Epsilon)
        return;
    
    // 교차점을 이은 선분을 Ring에 저장
    uint at;
    Meta.InterlockedAdd(4, 1, at); // Meta 버퍼 4번 바이트에 지금까지 저장된 선분 개수를 읽어 1을 더해서 at으로 저장
    if (VertexCapacity == 0)
        return;
    if (at >= MaxSegments)
    {
        Fail(2);
        return;
    }
    
    // 선분 하나에 128 바이트 (연결, loop 상태, cap 정보를 저장)
    Ring.Store3(at * 128, asuint(enter)); // 3D 좌표 저장 (12바이트)
    Ring.Store3(at * 128 + 12, asuint(exit));
    Ring.Store2(at * 128 + 24, asuint(Project(enter))); // 평면 투영 2D 좌표 저장 (8바이트)
    Ring.Store2(at * 128 + 32, asuint(Project(exit)));
    Ring.Store(at * 128 + 92, kind); // 선분 종류를 저장, 1: 삼각형을 가로지르는 선분, 2, 4: 각각 양, 음 쪽에 속한 삼각형의 edge 하나가 평면 위에 존재
}
