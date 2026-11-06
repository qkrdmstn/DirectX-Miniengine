#include "CutCommon.hlsli"

// 분할된 삼각형 결과를 실제 A/B 출력에 작성
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 현재 절단이 성립하는지 확인
    if ((Meta.Load(20) & 7) != 7)
        return;
    
    // 선분 유효성 검사
    uint i = id.x;
    uint segCount = min(Meta.Load(4), MaxSegments);
    if (i >= segCount) return;
    if (Ring.Load(i * 128 + 108) == 0) return;  // 루프에 안 들어가는 선분
    
    // 단면(cap)을 사용하지 않을 루프를 건너뛰고 해당 루프 집계
    uint leader = Ring.Load(i * 128 + 48), ignored;
    uint boundaryFlags = Ring.Load(i * 128 + 52);
    if (boundaryFlags != 0)                             // 이번 절단에서 나온 bad 종류를 모음
        Meta.InterlockedOr(28, boundaryFlags, ignored); 

    if ((boundaryFlags & 5) != 0)                       // 끊김(1)이나 교차(4)
    {
        Meta.InterlockedAdd(8, 1, ignored);             // cap이 없는 선분 수를 증가
        return;
    }
    if (Ring.Load(leader * 128 + 84) == 0)              // CapEars가 이 루프를 끝까지 메우지 못함
    {
        Meta.InterlockedAdd(24, 1, ignored);            // cap 실패 선분 수 증가.
        Meta.InterlockedAdd(8, 1, ignored);             
        return;
    }
    
    
    if (Ring.Load(i * 128 + 76) == 0)                   // 현재 선분 슬롯에 cap 삼각형이 있는지 확인
        return;
    uint3 corners = Ring.Load3(i * 128 + 64);           // 삼각형을 구성하는 세 선분 읽기
    
    // 각 선분의 세 시작점으로 정점 생성
    Vertex a = (Vertex) 0, b = (Vertex) 0, c = (Vertex) 0;
    a.Origin = b.Origin = c.Origin = uint4(0xffffffff, leader, i, 0);
    a.P = Point(corners.x);
    b.P = Point(corners.y);
    c.P = Point(corners.z);
    
    // 면적 0인 삼각형 건너뛰기
    if (length(cross(b.P - a.P, c.P - a.P)) <= Epsilon * Epsilon)   // 면적 0인 삼각형
        return;
    
    // 생성된 삼각형의 순회 순서를 맞춰준다. (Backface culling에 의해 안보일 수도 있기 때문)
    float3 normal = SafeNormal(Plane.xyz);
    if (dot(cross(b.P - a.P, c.P - a.P), normal) > 0)
    {
        Vertex temp = b;
        b = c;
        c = temp;
    }
    
    // A쪽 cap 작성
    a.N = b.N = c.N = -normal;
    a.T = b.T = c.T = float4(SafeNormal(ProjectU.xyz), -1);
    a.UV0 = a.UV1 = Project(a.P);
    b.UV0 = b.UV1 = Project(b.P);
    c.UV0 = c.UV1 = Project(c.P);
    EmitTriangle(0, Sections - 1, a, b, c);
    
    // B쪽 cap 작성
    a.N = b.N = c.N = normal;
    a.T.w = b.T.w = c.T.w = 1;
    EmitTriangle(1, Sections - 1, a, c, b);
    
    // 기록한 cap 삼각형 수
    Meta.InterlockedAdd(16, 1, ignored);
}
