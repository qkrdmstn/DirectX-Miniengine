
// 하나의 모델에 여러 메시(재질이 다르거나 부품이 다르거나)가 존재할 경우 각 메시를 Section 단위로 구분
// Cpp CutSection과 같은 구조이다.
struct Section
{
    // [입력] 원본 Data 버퍼에서 이 section(draw)을 읽는 위치
    uint VbOffset; // 정점 구간 시작 바이트
    uint Stride; // 정점 하나의 바이트 크기
    uint IbOffset; // 인덱스 구간 시작 바이트
    uint Index32; // 1이면 32비트 인덱스, 0이면 16비트
    uint StartIndex; // 이 draw의 첫 인덱스 번호
    uint BaseVertex; // 인덱스 값에 더할 정점 번호
    uint Triangles; // 이 draw의 삼각형 수
    uint ReservedPose; // 예약(옛 PG offset 자리), 읽지 않음

    // [입력 번호] 모든 section을 이어 붙인 전체 삼각형 번호 기준
    uint FirstTriangle; // 이 section의 첫 삼각형 번호

    // [출력] 출력 버퍼에서 이 section이 쓰는 구간
    uint VertexStart; // 출력 구간 시작 정점 번호
    uint VertexCapacity; // 출력 구간 최대 정점 수, 넘치면 Fail(2)

    // [형식] 정점 속성 비트
    uint Flags; // 원본 psoFlags: 4 탄젠트, 8 UV0, 16 UV1, 0x40 depth에 UV 포함

    // [출처] provenance 기록과 범위 검사용
    uint SourceSection; // 원본 revision의 mesh 번호
    uint SourceDraw; // 그 mesh 안의 draw 번호
    uint VertexCount; // 원본 정점 수, 인덱스 범위 검사(Fail(4))에 사용

    // [분류] 평면과의 관계를 CPU가 미리 판정한 결과
    uint Reserved; // 0 걸침(자름), 1 전부 A쪽, 2 전부 B쪽(자르지 않고 통째로 복사)

    // 원본 버퍼는 각 section의 로컬 좌표로 저장돼 있다. 
    // -> 모든 section을 MeshComponent 하나의 공통 좌표로 표현하는 component 좌표계로 변환 (노드 행렬과  곱함)
    row_major float3x4 SectionToComponent; // 위치
    row_major float3x4 NormalToComponent; // 법선, 탄젠트
};

// 절단 과정에 쓰이는 상수
cbuffer Parameters : register(b0)
{
    // 평면 정보
    float4 Plane; // 평면 변수 (a, b, c, d) => ax + by + cz + d = 0
    float4x4 LocalToRectangle;  // 컴포넌트 좌표계의 정점을 컴포넌트 좌표 -> 월드 좌표 -> 평면 로컬 좌표로 변환하는 행렬
    float4 ProjectU, ProjectV; // 평면 위의 직교 좌표축 (교차점 평면 투영에 사용)
    float Epsilon, HalfWidth, HalfHeight; // 오차, 평면 크기
    
    // 절단 작업 크기
    uint Triangles;
    uint Sections, VertexCapacity;
    
    // 출력 버퍼 배치
    uint DepthOffset, IndexOffset;
    
    // Ring 버퍼 배치
    uint MaxSegments, HashMask, HashOffset, HashLinksOffset;
};

ByteAddressBuffer Source : register(t0); // job->Input->GetDataBuffer() : Revision이 가리키고 있는 정점 버퍼(SurfaceVB, depthVB, IB)
StructuredBuffer<Section> Inputs : register(t2); // job->Descripors: CutSection 배열
RWByteAddressBuffer DataA : register(u0); // Output[0]->Data: 절단 그룹 중 A방향 정점 데이터(SurfaceVB, depthVB, IB)
RWByteAddressBuffer DataB : register(u1); // Output[1]->Data: 절단 그룹 중 B방향 정점 데이터

// job->Metadata: 모든 단계가 원자 연산으로 개수, 각 작업의 상태를 저장
// 끝나면 이 버퍼만 CPU로 복사(StatusReadback)해서 Poll/HasSplit/BuildResults가 읽음
RWByteAddressBuffer Meta : register(u2); 

// job->Scratch: 삼각형-평면 교차 시 생성되는 선분 모음, 출력은 아니지만 계산 중간 버퍼 형태이다.(CutJob과 함께 버려짐)
// 앞부분 = 끝점(3d, 2d), 선분 종류, 앞뒤 연결, loop 대표, 상태, cap 작업)
// 뒷부분 = 끝점 해시 칸, 해시 연결 (HashOffset, HashLinksOffset부터)
// 해시 테이블, 하나의 셀에 대해 여러 정점을 링크로 담음
RWByteAddressBuffer Ring : register(u3);

// 테스트 코드에서만 사용
RWByteAddressBuffer OriginA : register(u4); //Output[0]->Provenance
RWByteAddressBuffer OriginB : register(u5); //Output[1]->Provenance

struct Vertex
{
    float3 P, N;
    float4 T;
    float2 UV0, UV1;
    uint4 Origin;
    float Parameter;
    uint TriangleId;
};

// 잘못된 후보를 게시하지 않도록 fatal 비트를 설정한다.
void Fail(uint flag)
{
    uint ignored;
    Meta.InterlockedOr(0, flag, ignored);
}

// 정렬 가능한 float 표현으로 GPU AABB의 최소/최대를 원자 갱신한다.
uint Ordered(float x)
{
    uint b = asuint(x);
    return (b & 0x80000000) ? ~b : (b ^ 0x80000000);
}

// 원본 버퍼 압축/해제 함수
// uint 하나로 압축된 법선, 탄젠트를 float4로 해제
float4 Frame(uint p)
{
    return float4(uint4(p, p >> 10, p >> 20, p >> 30) & uint4(1023, 1023, 1023, 3)) / float4(1023, 1023, 1023, 3) * 2 - 1;
}

// 생성 표면의 packed tangent/normal 압축
uint PackFrame(float4 v)
{
    uint4 a = uint4(round(saturate(v * .5 + .5) * float4(1023, 1023, 1023, 3)));
    return a.x | (a.y << 10) | (a.z << 20) | (a.w << 30);
}

// 압축된 UV를 해제
float2 UV(uint v)
{
    return float2(f16tof32(v & 65535), f16tof32(v >> 16));
}

// UV를 압축
uint PackUV(float2 v)
{
    return f32tof16(v.x) | (f32tof16(v.y) << 16);
}

// IB에서 현재 section의 i번째 인덱스가 가리키는 정점 번호를 반환
uint Index(Section s, uint i)
{
    uint at = s.StartIndex + i;
    uint word = Source.Load(s.IbOffset + (s.Index32 ? at * 4 : (at / 2) * 4));
    return (s.Index32 ? word : ((word >> ((at & 1) * 16)) & 65535)) + s.BaseVertex;
}

// 원본 버퍼에서 정점을 디코드하고, section 행렬로 해당 정점을 컴포넌트 로컬 좌표계로 옮긴 정점을 만들어 반환한다.
Vertex ReadVertex(Section s, uint i)
{
    // PreparePoseCS와 같은 packed 프레임 및 tangent 기본값을 읽는다.
    Vertex v = (Vertex) 0;
    v.Origin = uint4(s.SourceSection, s.SourceDraw, i, i);
    uint a = s.VbOffset + i * s.Stride;
    float3 position = asfloat(Source.Load3(a));
    float3 normal = Frame(Source.Load(a + 12)).xyz;
    float4 tangent = (s.Flags & 4) ? Frame(Source.Load(a + 16)) : float4(1, 0, 0, 1);

    // N/T는 보간 전 길이를 유지하고 handedness는 변환하지 않는다.
    v.P = mul(s.SectionToComponent, float4(position, 1));
    v.N = mul((float3x3) s.NormalToComponent, normal);
    v.T = float4(mul((float3x3) s.NormalToComponent, tangent.xyz), tangent.w);

    // tangent 존재 여부에 맞춰 UV 시작 주소를 구한다.
    uint uv = a + 16 + ((s.Flags & 4) ? 4 : 0);
    v.UV0 = (s.Flags & 8) ? UV(Source.Load(uv)) : 0;
    v.UV1 = (s.Flags & 16) ? UV(Source.Load(uv + ((s.Flags & 8) ? 4 : 0))) : v.UV0;
    return v;
}

// edge의 두 정점 a와 b 사이의 교차점 v를 선형 보간으로 생성
Vertex Blend(Vertex a, Vertex b, float t)
{
    Vertex v;
    v.P = lerp(a.P, b.P, t);
    v.N = lerp(a.N, b.N, t);
    v.T = lerp(a.T, b.T, t);
    v.UV0 = lerp(a.UV0, b.UV0, t);
    v.UV1 = lerp(a.UV1, b.UV1, t);
    v.Origin = uint4(a.Origin.xy, a.Origin.z, b.Origin.z);
    v.Parameter = t;
    v.TriangleId = a.TriangleId;
    return v;
}

// edge a-b가 평면과 만나는 교차점을 생성 (두 정점의 평면 법선 방향 성분의 비율로 edge의 교차점 검출)
// 공유 변의 방향에 무관하게 같은 산술 순서로 교점을 만들어 오차를 줄인다.
Vertex IntersectEdge(Vertex a, Vertex b, float da, float db)
{
     // ex) 정점 a와 b를 공유하는 두 삼각형: 삼각형 A: (Edge a->b), 삼각형 B (Edge b->a)이 있을 때, 
    // Edge a-b 순회 순서(계산 순서)에 따라 float 산술 오차가 발생할 수 있다.
    // 하나의 edge를 공유하는 두 삼각형 사이에 edge 방향이 다를 수 있어서 이를 맞춰준다. 
    bool reverse = a.P.x > b.P.x || (a.P.x == b.P.x && (a.P.y > b.P.y || (a.P.y == b.P.y && a.P.z > b.P.z)));
    if (reverse)
    {
        Vertex tmp = a;
        a = b;
        b = tmp;
        float t = da;
        da = db;
        db = t;
    }
    return Blend(a, b, da / (da - db));
}

// 유효하지 않은 법선의 NaN 전파를 막는다.
float3 SafeNormal(float3 n)
{
    return dot(n, n) > 1e-20 ? normalize(n) : float3(0, 1, 0);
}

// Vertex 데이터를 정규화 및 압축해 DataA/B에 surfaceVB, depthVB, IB 형태로 작성, OriginA/B에 출처를 작성
void WriteVertex(uint side, Section s, uint local, Vertex v)
{
    // 정점 프레임을 정규화하고 surface, depth 주소를 계산한다.
    uint global = s.VertexStart + local;
    v.N = SafeNormal(v.N);
    v.T.xyz = SafeNormal(v.T.xyz);
    v.T.w = v.T.w < 0 ? -1 : 1;
    uint a = global * 28;
    uint stride = (s.Flags & 0x40) ? 16 : 12;
    uint d = DepthOffset + s.VertexStart * 16 + local * stride;

    // 선택한 출력면의 packed Data와 인덱스를 기록한다.
    if (side == 0)
    {
        // surfaceVB 쓰기
        DataA.Store3(a, asuint(v.P));
        DataA.Store(a + 12, PackFrame(float4(v.N, 1)));
        uint uv = a + 16;
        if (s.Flags & 4)
        {
            DataA.Store(uv, PackFrame(v.T));
            uv += 4;
        }
        DataA.Store(uv, PackUV(v.UV0));
        if (s.Flags & 16)
            DataA.Store(uv + 4, PackUV(v.UV1));
        
        // depthVB 쓰기
        DataA.Store3(d, asuint(v.P));
        if (stride == 16)
            DataA.Store(d + 12, PackUV(v.UV0));
        
        // IB 쓰기
        DataA.Store(IndexOffset + global * 4, local);
    }
    else
    {
        // surfaceVB 쓰기
        DataB.Store3(a, asuint(v.P));
        DataB.Store(a + 12, PackFrame(float4(v.N, 1)));
        uint uv = a + 16;
        if (s.Flags & 4)
        {
            DataB.Store(uv, PackFrame(v.T));
            uv += 4;
        }
        DataB.Store(uv, PackUV(v.UV0));
        if (s.Flags & 16)
            DataB.Store(uv + 4, PackUV(v.UV1));
        
        // depthVB 쓰기
        DataB.Store3(d, asuint(v.P));
        if (stride == 16)
            DataB.Store(d + 12, PackUV(v.UV0));
        
        // IB 쓰기
        DataB.Store(IndexOffset + global * 4, local);
    }

    // 부모 revision의 section·draw·정점과 교차 계수를 보존한다.
    uint originAddress = global * 32;
    if (side == 0)
    {
        OriginA.Store4(originAddress, v.Origin);
        OriginA.Store4(originAddress + 16, uint4(asuint(v.Parameter), v.TriangleId, 0, 0));
    }
    else
    {
        OriginB.Store4(originAddress, v.Origin);
        OriginB.Store4(originAddress + 16, uint4(asuint(v.Parameter), v.TriangleId, 0, 0));
    }
}

// 정점 3개를 입력받아 삼각형을 출력 버퍼에 저장하는 함수
void EmitTriangle(uint side, uint section, Vertex a, Vertex b, Vertex c)
{
    Section s = Inputs[section];
    uint start;
    Meta.InterlockedAdd(80 + (side * Sections + section) * 4, 3, start);
    // 사전 집계는 동일한 삼각형 판정 뒤 카운터만 남긴다.
    if (VertexCapacity == 0)
        return;
    if (start + 3 > s.VertexCapacity)
    {
        Fail(2);
        return;
    }
    WriteVertex(side, s, start, a);
    WriteVertex(side, s, start + 1, b);
    WriteVertex(side, s, start + 2, c);
}

// 단면의 직교 2D 좌표를 생성
float2 Project(float3 p)
{
    return float2(dot(ProjectU, float4(p, 1)), dot(ProjectV, float4(p, 1)));
}

// 단면 점과 연결 정보를 읽는다.
float3 Point(uint i)
{
    return asfloat(Ring.Load3(i * 128));
}

// 선분 시작점의 평면 좌표를 읽는다.
float2 Point2(uint i)
{
    return asfloat(Ring.Load2(i * 128 + 24));
}

// GPU 경계의 다음 선분을 읽는다.
uint Next(uint i)
{
    return Ring.Load(i * 128 + 40);
}

// GPU 경계의 이전 선분을 읽는다.
uint Prev(uint i)
{
    return Ring.Load(i * 128 + 44);
}

// 2D 외적의 부호 있는 크기를 계산한다.
float Cross2(float2 a, float2 b)
{
    return a.x * b.y - a.y * b.x;
}
