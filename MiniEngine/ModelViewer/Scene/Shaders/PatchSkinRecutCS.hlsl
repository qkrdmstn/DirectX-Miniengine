cbuffer Parameters : register(b0)
{
    float4x4 InverseK;
    uint FirstVertex, VertexCount, Flags, Stride;
    uint RigidJoint, ParentSectionCount, Pad0, Pad1;
};
ByteAddressBuffer Parent : register(t0);
ByteAddressBuffer Data : register(t1);
ByteAddressBuffer Provenance : register(t2);
StructuredBuffer<uint> ParentFirstVertex : register(t3);
RWByteAddressBuffer Result : register(u0);
// 확장 skin 정점 배치: 정점 끝에서 거꾸로 센 바이트 거리(Stride 기준).
//   -96 내부 교점 표시(uint) | -92 보간 t(float) | -16 joints+weights
static const uint ExtInternal = 96;

// cutter와 같은 UNORM 프레임을 방향 벡터로 복원한다.
float4 Frame(uint packed)
{
    // 각 성분의 비트를 분리하고 부호 있는 범위로 되돌린다.
    return float4(uint4(packed, packed >> 10, packed >> 20, packed >> 30)
        & uint4(1023, 1023, 1023, 3)) / float4(1023, 1023, 1023, 3) * 2 - 1;
}

// PatchSkinBuildCS의 RotateFrame과 같이 InverseK의 cofactor로 방향을 되돌리고 w를 유지한다.
uint RotateFrame(uint packed)
{
    // InverseK 3x3의 cofactor 행렬을 만든다(크기는 정규화로, 부호는 행렬식으로 맞춘다).
    float3x3 m = (float3x3)InverseK;
    float3x3 cofactor = float3x3(cross(m[1], m[2]), cross(m[2], m[0]), cross(m[0], m[1]));
    float handedness = dot(m[0], cofactor[0]) < 0 ? -1 : 1;

    // handedness를 유지하고 K 슬롯 bind 공간 방향을 정규화한다.
    float4 frame = Frame(packed);
    float3 direction = mul(cofactor, frame.xyz) * handedness;
    frame.xyz = dot(direction, direction) > 1e-20 ? normalize(direction) : float3(0, 1, 0);
    uint4 value = uint4(round(saturate(frame * .5 + .5) * float4(1023, 1023, 1023, 3)));
    return value.x | (value.y << 10) | (value.z << 20) | (value.w << 30);
}

// 복사 정점은 부모 skin 레코드를 통째로 옮기고 split·cap 정점은 K 슬롯 bind 레코드를 만든다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 결과 Data 전역 정점 번호와 provenance를 읽는다.
    uint local = id.x + id.y * (65535 * 64);
    if (local >= VertexCount)
        return;
    uint vertex = FirstVertex + local;
    uint4 origin = Provenance.Load4(vertex * 32);
    uint output = vertex * Stride;

    // 복사 판정이 맞고 부모 section이 유효하면 부모 레코드를 Stride 바이트 통째로 복사한다.
    bool copied = origin.x != 0xffffffff && origin.z == origin.w && origin.x < ParentSectionCount;
    if (copied)
    {
        uint parentAt = (ParentFirstVertex[origin.x] + origin.z) * Stride;
        for (uint copyAt = 0; copyAt < Stride; copyAt += 4)
            Result.Store(output + copyAt, Parent.Load(parentAt + copyAt));
        return;
    }

    // 새 절단면 정점은 위치·방향을 K 슬롯 bind 공간으로 되돌린다.
    uint address = vertex * 28;
    Result.Store3(output, asuint(mul(InverseK, float4(asfloat(Data.Load3(address)), 1)).xyz));
    Result.Store(output + 12, RotateFrame(Data.Load(address + 12)));

    // 결과 Data의 tangent와 uv를 Flags 순서로 복사한다(tangent는 방향만 되돌린다).
    uint at = output + 16;
    if (Flags & 4)
    {
        Result.Store(at, RotateFrame(Data.Load(address + 16)));
        at += 4;
    }
    if (Flags & 8)
    {
        Result.Store(at, Data.Load(address + 20));
        at += 4;
    }
    if (Flags & 16)
        Result.Store(at, Data.Load(address + 24));

    // 내부 교점 표시는 끄고 K 슬롯 하나에 weight 1로 붙인다.
    Result.Store2(output + Stride - ExtInternal, uint2(0, 0));
    Result.Store4(output + Stride - 16, uint4(RigidJoint, 0, 65535, 0));
}
