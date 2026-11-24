cbuffer Parameters : register(b0)
{
    float4x4 InverseNode;
    uint FirstVertex, VertexCount, Flags, Stride;
    uint DrawCount, RigidJoint, PatchTriangles, Follow;
};
struct DrawInput
{
    uint Section, Draw, VbOffset, SourceStride;
    uint IbOffset, Index32, StartIndex, BaseVertex;
    uint SourceFlags, StartJoint;
};
ByteAddressBuffer Source : register(t0);
ByteAddressBuffer TriangleSources : register(t1);
ByteAddressBuffer Data : register(t2);
ByteAddressBuffer Provenance : register(t3);
StructuredBuffer<DrawInput> Inputs : register(t4);
struct Joint
{
    float4x4 PosMatrix;
    float4x3 NrmMatrix;
};
StructuredBuffer<Joint> JointsAtCut : register(t5);
RWByteAddressBuffer Result : register(u0);
// 확장 skin 정점 배치: 정점 끝에서 거꾸로 센 바이트 거리(Stride 기준).
//   -96 끝점 보간 종류(uint: 0 없음, 1 내부 교점, 2 실제 분리 rim, 3 cap) | -92 보간 t(float)
//   -88 끝점0 36B | -52 끝점1 36B | -16 기존 joints+weights
//   끝점 36B = 위치 12 + 법선 4 | 탄젠트 4(+16) | joints 8(+20) | weights 8(+28)
static const uint ExtInternal = 96;
static const uint ExtEndpoint0 = 88;
static const uint ExtEndpoint1 = 52;
static const uint EndpointTangent = 16;
static const uint EndpointSkin = 20;

// cutter와 같은 UNORM 프레임을 방향 벡터로 복원한다.
float4 Frame(uint packed)
{
    // 각 성분의 비트를 분리하고 부호 있는 범위로 되돌린다.
    return float4(uint4(packed, packed >> 10, packed >> 20, packed >> 30)
        & uint4(1023, 1023, 1023, 3)) / float4(1023, 1023, 1023, 3) * 2 - 1;
}

// PreparePoseCS가 방향에 곱하는 inverse-transpose(N)을 되돌려 모델 공간 방향으로 압축한다.
// 되돌리는 행렬은 transpose(N) = inverse-transpose(InverseNode)이며, cofactor 행렬로 구한다.
uint RotateFrame(uint packed)
{
    // InverseNode 3x3의 cofactor 행렬을 만든다(크기는 정규화로, 부호는 행렬식으로 맞춘다).
    float3x3 m = (float3x3)InverseNode;
    float3x3 cofactor = float3x3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    float handedness = dot(m[0], cofactor[0]) < 0 ? -1 : 1;

    // handedness를 유지하고 모델 공간 방향을 정규화한다.
    float4 frame = Frame(packed);
    float3 direction = mul(cofactor, frame.xyz) * handedness;
    frame.xyz = dot(direction, direction) > 1e-20 ? normalize(direction) : float3(0, 1, 0);
    uint4 value = uint4(round(saturate(frame * .5 + .5) * float4(1023, 1023, 1023, 3)));
    return value.x | (value.y << 10) | (value.z << 20) | (value.w << 30);
}

// PatchExtractCS::Index와 동일하게 원본 IB와 baseVertex를 해석한다.
uint Index(DrawInput draw, uint i)
{
    // R16 인덱스도 DWORD 정렬 주소에서 읽는다.
    uint at = draw.StartIndex + i;
    uint word = Source.Load(draw.IbOffset + (draw.Index32 ? at * 4 : (at / 2) * 4));
    return (draw.Index32 ? word : ((word >> ((at & 1) * 16)) & 65535)) + draw.BaseVertex;
}

// 추출 패치 꼭짓점을 원본 draw와 bind 주소로 대응시킨다.
uint SourceAddress(uint vertex, out DrawInput draw)
{
    // 기존 sourceTriangle 출처와 corner를 그대로 사용한다.
    uint3 sourceTriangle = TriangleSources.Load3((vertex / 3) * 12);
    uint d = 0;
    while (d + 1 < DrawCount && any(uint2(Inputs[d].Section, Inputs[d].Draw) != sourceTriangle.xy))
        d++;
    draw = Inputs[d];
    return draw.VbOffset + Index(draw, sourceTriangle.z * 3 + vertex % 3) * draw.SourceStride;
}

// 끝점의 원본 법선을 절단 시점 팔레트(JointsAtCut)로 스키닝해 PreparePoseCS의 SkinEndpoint와 같은 공간의 UNORM 법선으로 압축한다.
uint CutTimeNormal(uint address, DrawInput draw)
{
    // 원본 법선과 정규화 weight, 전체 팔레트 인덱스를 읽는다.
    float3 normal = Frame(Source.Load(address + 12)).xyz;
    uint4 skin = Source.Load4(address + draw.SourceStride - 16);
    uint4 indices = uint4(skin.x & 65535, skin.x >> 16, skin.y & 65535, skin.y >> 16) + draw.StartJoint;
    float4 weights = float4(skin.z & 65535, skin.z >> 16, skin.w & 65535, skin.w >> 16);
    weights /= max(dot(weights, 1), 1);

    // 네 영향의 법선 행렬을 섞어 법선을 변환하고 정규화한다.
    float4x3 skinNormal = JointsAtCut[indices.x].NrmMatrix * weights.x + JointsAtCut[indices.y].NrmMatrix * weights.y
        + JointsAtCut[indices.z].NrmMatrix * weights.z + JointsAtCut[indices.w].NrmMatrix * weights.w;
    float3 skinned = mul(skinNormal, normal).xyz;
    skinned = dot(skinned, skinned) > 1e-20 ? normalize(skinned) : float3(0, 1, 0);

    // UNORM 10-10-10-2로 압축한다.
    uint3 value = uint3(round(saturate(skinned * .5 + .5) * 1023));
    return value.x | (value.y << 10) | (value.z << 20) | (3u << 30);
}

// 끝점은 원본 속성과 전체 팔레트 인덱스를 별도로 보관한다. cap 끝점은 탄젠트 칸에 절단 시점 법선(K 프레임)을 넣는다.
void StoreEndpoint(uint output, uint vertex, bool cap)
{
    // 두 끝점의 weight를 합치지 않고 각각의 skin 입력을 복사한다.
    DrawInput draw;
    uint address = SourceAddress(vertex, draw);
    Result.Store4(output, Source.Load4(address));
    Result.Store(output + EndpointTangent, (draw.SourceFlags & 4) ? Source.Load(address + 16) : 0xe00803ff);

    // cap은 PreparePoseCS가 끝점의 절단 시점 → 현재 법선 회전을 구할 수 있게 절단 시점 팔레트로 스키닝한 법선을 기록한다.
    if (cap)
        Result.Store(output + EndpointTangent, CutTimeNormal(address, draw));
    uint4 skin = Source.Load4(address + draw.SourceStride - 16);
    uint4 joints = uint4(skin.x & 65535, skin.x >> 16, skin.y & 65535, skin.y >> 16) + draw.StartJoint;
    Result.Store4(output + EndpointSkin, uint4(joints.x | (joints.y << 16), joints.z | (joints.w << 16), skin.zw));
}

// 복사 정점은 원본 bind로 복원한다. 내부 교점(1)과, 조각 플래그가 켜진 실제 분리 rim(2)·cap(3)에는 두 끝점의 원본 skin 입력을 덧붙인다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // provenance의 z/w는 추출 패치 VB의 끝점 번호이며 cap 표식을 먼저 검사한다.
    uint local = id.x + id.y * (65535 * 64);
    if (local >= VertexCount)
        return;
    uint vertex = FirstVertex + local;
    uint4 origin = Provenance.Load4(vertex * 32);
    bool copied = origin.x != 0xffffffff && origin.z == origin.w;
    uint output = vertex * Stride;
    uint address = vertex * 28;
    uint sourceFlags = 31;
    uint startJoint = 0;
    uint4 skin = uint4(RigidJoint, 0, 65535, 0);

    // 복사된 패치 정점을 section/draw/sourceTriangle과 corner로 원본 VB에 대응시킨다.
    if (copied)
    {
        DrawInput draw;
        address = SourceAddress(origin.z, draw);
        sourceFlags = draw.SourceFlags;
        startJoint = draw.StartJoint;
        skin = Source.Load4(address + draw.SourceStride - 16);
        Result.Store3(output, Source.Load3(address));
        Result.Store(output + 12, Source.Load(address + 12));
    }
    else
    {
        Result.Store3(output, asuint(mul(InverseNode, float4(asfloat(Data.Load3(address)), 1)).xyz));
        Result.Store(output + 12, RotateFrame(Data.Load(address + 12)));
    }

    // section별 flags의 합집합을 사용하고 없는 속성에는 기본값을 넣는다.
    uint tangent = (sourceFlags & 4) ? (copied ? Source.Load(address + 16) : RotateFrame(Data.Load(address + 16))) : 0xe00803ff;
    uint uvAt = address + 16 + ((sourceFlags & 4) ? 4 : 0);
    uint uv0 = (sourceFlags & 8) ? (copied ? Source.Load(uvAt) : Data.Load(uvAt)) : 0;
    uint uv1 = (sourceFlags & 16) ? (copied ? Source.Load(uvAt + ((sourceFlags & 8) ? 4 : 0)) : Data.Load(uvAt + 4)) : uv0;
    uint at = output + 16;
    if (Flags & 4)
    {
        Result.Store(at, tangent);
        at += 4;
    }
    if (Flags & 8)
    {
        Result.Store(at, uv0);
        at += 4;
    }
    if (Flags & 16)
        Result.Store(at, uv1);

    // 끝점 보간 종류를 정한다: 같은 몸체 교점은 1, 조각 플래그가 켜진 실제 rim은 2, cap은 3이다.
    uint sourceTriangle = Provenance.Load(vertex * 32 + 20);
    uint interpolation = 0;
    if (origin.x != 0xffffffff && !copied && sourceTriangle < PatchTriangles)
    {
        DrawInput bodies = Inputs[DrawCount + sourceTriangle];
        if (bodies.Section != 0xffffffff)
            interpolation = bodies.Section == bodies.Draw ? 1 : (Follow ? 2 : 0);
    }
    else if (origin.x == 0xffffffff && Follow && origin.z < PatchTriangles * 3 && origin.w < PatchTriangles * 3)
    {
        // cap의 (e0, e1)은 split과 같은 추출 패치 VB 번호라서 SourceAddress가 section/draw를 그대로 찾는다.
        interpolation = 3;
    }
    Result.Store2(output + Stride - ExtInternal, uint2(interpolation, Provenance.Load(vertex * 32 + 16)));
    if (interpolation)
    {
        StoreEndpoint(output + Stride - ExtEndpoint0, origin.z, interpolation == 3);
        StoreEndpoint(output + Stride - ExtEndpoint1, origin.w, interpolation == 3);
    }

    // 전체 팔레트를 직접 참조하도록 네 joint 모두에 section startJoint를 더한다.
    uint4 joints = uint4(skin.x & 65535, skin.x >> 16, skin.y & 65535, skin.y >> 16) + startJoint;
    Result.Store4(output + Stride - 16, uint4(joints.x | (joints.y << 16), joints.z | (joints.w << 16), skin.zw));
}
