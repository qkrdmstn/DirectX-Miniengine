cbuffer Parameters : register(b0)
{
    float4x4 InverseNode;
    uint FirstVertex, VertexCount, Flags, Stride;
    uint DrawCount, RigidJoint;
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
RWByteAddressBuffer Result : register(u0);

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

// 복사 정점은 원본 bind 속성으로 복원하고 새 정점은 강체 슬롯 K에 붙인다.
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

    // 복사된 패치 정점을 section/draw/triangle과 corner로 원본 VB에 대응시킨다.
    if (copied)
    {
        uint3 sourceTriangle = TriangleSources.Load3((origin.z / 3) * 12);
        uint d = 0;
        while (d + 1 < DrawCount && any(uint2(Inputs[d].Section, Inputs[d].Draw) != sourceTriangle.xy))
            d++;
        DrawInput draw = Inputs[d];
        address = draw.VbOffset + Index(draw, sourceTriangle.z * 3 + origin.z % 3) * draw.SourceStride;
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

    // 전체 팔레트를 직접 참조하도록 네 joint 모두에 section startJoint를 더한다.
    uint4 joints = uint4(skin.x & 65535, skin.x >> 16, skin.y & 65535, skin.y >> 16) + startJoint;
    Result.Store4(output + Stride - 16, uint4(joints.x | (joints.y << 16), joints.z | (joints.w << 16), skin.zw));
}
