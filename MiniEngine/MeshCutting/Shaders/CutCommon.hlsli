struct Section
{
    uint VbOffset, Stride, IbOffset, Index32;
    uint StartIndex, BaseVertex, Triangles, ReservedPose; // C++과 같은 예약 DWORD이며 읽지 않는다.
    uint FirstTriangle, VertexStart, VertexCapacity, Flags;
    uint SourceSection, SourceDraw, VertexCount, Reserved;
    // C++의 행 우선 3x4와 일치한다. 위치는 mul(S, float4(p, 1)) 규약이다.
    row_major float3x4 SectionToComponent;
    // 역전치 3x3의 네 번째 열은 0이다. N/T는 mul((float3x3)Nm, n) 규약이다.
    row_major float3x4 NormalToComponent;
};
cbuffer Parameters : register(b0)
{
    float4 Plane;
    float4x4 LocalToRectangle;
    float4 ProjectU, ProjectV;
    float Epsilon, HalfWidth, HalfHeight;
    uint Triangles;
    uint Sections, VertexCapacity, DepthOffset, IndexOffset;
    uint MaxSegments, HashMask, HashOffset, HashLinksOffset;
};
ByteAddressBuffer Source : register(t0);
StructuredBuffer<Section> Inputs : register(t2);
RWByteAddressBuffer DataA : register(u0);
RWByteAddressBuffer DataB : register(u1);
RWByteAddressBuffer Meta : register(u2);
// Meta DWORD: section 카운터 [20,20+2*Sections), KDop 38개 뒤에 cap 진단 5개를 추가한다.
// [58+2*Sections,+1,+2]는 각각 capRounds, capMarked, capRemoved이며 기존 오프셋은 유지한다.
// [61+2*Sections,+1]는 각각 capMaxLoop, capMappedLoops다.
// [63+2*Sections]는 큰 loop scan map 구간을 예약하는 원자 cursor다.
RWByteAddressBuffer Ring : register(u3);
RWByteAddressBuffer OriginA : register(u4);
RWByteAddressBuffer OriginB : register(u5);

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

// 렌더 입력의 UNORM 프레임을 복원한다.
float4 Frame(uint p)
{
    return float4(uint4(p, p >> 10, p >> 20, p >> 30) & uint4(1023, 1023, 1023, 3)) / float4(1023, 1023, 1023, 3) * 2 - 1;
}

// 생성 표면의 packed tangent/normal을 기록한다.
uint PackFrame(float4 v)
{
    uint4 a = uint4(round(saturate(v * .5 + .5) * float4(1023, 1023, 1023, 3)));
    return a.x | (a.y << 10) | (a.z << 20) | (a.w << 30);
}

// 반정밀도 UV를 읽는다.
float2 UV(uint v)
{
    return float2(f16tof32(v & 65535), f16tof32(v >> 16));
}

// 반정밀도 UV를 기록한다.
uint PackUV(float2 v)
{
    return f32tof16(v.x) | (f32tof16(v.y) << 16);
}

// 원본 인덱스 형식을 해석한다.
uint Index(Section s, uint i)
{
    uint at = s.StartIndex + i;
    uint word = Source.Load(s.IbOffset + (s.Index32 ? at * 4 : (at / 2) * 4));
    return (s.Index32 ? word : ((word >> ((at & 1) * 16)) & 65535)) + s.BaseVertex;
}

// revision 정점을 디코드하고 section 행렬로 component-local 속성을 복원한다.
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

// 교차점 속성을 같은 계수로 보간한다.
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

// 공유 변의 방향에 무관하게 같은 산술 순서로 교점을 만들어 seam 오차를 줄인다.
Vertex IntersectEdge(Vertex a, Vertex b, float da, float db)
{
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

// 고정 할당 구간에 서로 겹치지 않는 triangle-corner 출력을 쓴다.
void WriteVertex(uint side, Section s, uint local, Vertex v)
{
    // 정점 프레임을 정규화하고 표면·depth 주소를 계산한다.
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
        DataA.Store3(d, asuint(v.P));
        if (stride == 16)
            DataA.Store(d + 12, PackUV(v.UV0));
        DataA.Store(IndexOffset + global * 4, local);
    }
    else
    {
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
        DataB.Store3(d, asuint(v.P));
        if (stride == 16)
            DataB.Store(d + 12, PackUV(v.UV0));
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
// 단면의 직교 2D 좌표를 만든다.
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
