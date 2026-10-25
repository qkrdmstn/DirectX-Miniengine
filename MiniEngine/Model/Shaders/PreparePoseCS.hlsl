cbuffer DecodeConstants : register(b0)
{
    uint SourceOffset;
    uint SourceStride;
    uint VertexCount;
    uint Flags;
};

cbuffer NodeConstants : register(b1)
{
    float4x4 NodeMatrix;
    float3x3 NodeNormalMatrix;
};

struct Joint
{
    float4x4 PosMatrix;
    float4x3 NrmMatrix;
};

ByteAddressBuffer Source : register(t0);
StructuredBuffer<Joint> Joints : register(t1);
RWByteAddressBuffer Result : register(u0);

// 렌더 정점의 압축 UNORM을 기존 VS와 같은 [-1,1] 성분으로 복원한다.
float4 DecodeFrame(uint packed)
{
    return float4(uint4(packed, packed >> 10, packed >> 20, packed >> 30)
        & uint4(1023, 1023, 1023, 3)) / float4(1023, 1023, 1023, 3) * 2 - 1;
}

// static·skeletal을 같은 posed local 형식으로 변환하며 원본 정점 번호를 유지한다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    uint vertex = id.x + id.y * (65535 * 64);
    if (vertex >= VertexCount)
        return;
    uint address = SourceOffset + vertex * SourceStride;
    float4 position = float4(asfloat(Source.Load3(address)), 1);
    float3 normal = DecodeFrame(Source.Load(address + 12)).xyz;
    float4 tangent = (Flags & 4) ? DecodeFrame(Source.Load(address + 16)) : float4(1, 0, 0, 1);
    if (Flags & 0x100)
    {
        uint4 packed = Source.Load4(address + SourceStride - 16);
        uint4 indices = uint4(packed.x & 65535, packed.x >> 16, packed.y & 65535, packed.y >> 16);
        float4 weights = float4(packed.z & 65535, packed.z >> 16, packed.w & 65535, packed.w >> 16);
        weights /= max(dot(weights, 1), 1);
        float4x4 skin = Joints[indices.x].PosMatrix * weights.x + Joints[indices.y].PosMatrix * weights.y
            + Joints[indices.z].PosMatrix * weights.z + Joints[indices.w].PosMatrix * weights.w;
        float4x3 skinNormal = Joints[indices.x].NrmMatrix * weights.x + Joints[indices.y].NrmMatrix * weights.y
            + Joints[indices.z].NrmMatrix * weights.z + Joints[indices.w].NrmMatrix * weights.w;
        position = mul(skin, position);
        normal = mul(skinNormal, normal).xyz;
        tangent.xyz = mul(skinNormal, tangent.xyz).xyz;
    }
    uint outputAddress = vertex * 40;
    Result.Store3(outputAddress, asuint(mul(NodeMatrix, position).xyz));
    Result.Store3(outputAddress + 12, asuint(mul(NodeNormalMatrix, normal)));
    Result.Store4(outputAddress + 24, asuint(float4(mul(NodeNormalMatrix, tangent.xyz), tangent.w)));
}
