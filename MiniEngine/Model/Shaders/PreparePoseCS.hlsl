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
// 확장 skin 정점 배치: 정점 끝에서 거꾸로 센 바이트 거리(Stride 기준).
//   -96 끝점 보간 종류(uint: 0 없음, 1 내부 교점, 2 실제 분리 rim, 3 cap) | -92 보간 t(float)
//   -88 끝점0 36B | -52 끝점1 36B | -16 기존 joints+weights
//   끝점 36B = 위치 12 + 법선 4 | 탄젠트 4(+16, cap은 K 법선을 절단 시점 팔레트의 역변환으로 끝점 스키닝 이전 방향으로 되돌린 값) | joints 8(+20) | weights 8(+28)
static const uint ExtFlag = 0x200;
static const uint ExtInternal = 96;
static const uint ExtT = 92;
static const uint ExtEndpoint0 = 88;
static const uint ExtEndpoint1 = 52;
static const uint EndpointTangent = 16;
static const uint EndpointSkin = 20;

// 렌더 정점의 압축 UNORM을 기존 VS와 같은 [-1,1] 성분으로 복원한다.
float4 DecodeFrame(uint packed)
{
    return float4(uint4(packed, packed >> 10, packed >> 20, packed >> 30)
        & uint4(1023, 1023, 1023, 3)) / float4(1023, 1023, 1023, 3) * 2 - 1;
}

// 한 끝점의 원래 weight로 위치와 방향을 독립적으로 스키닝한다.
void SkinEndpoint(uint address, uint skinAddress, out float4 position, out float3 normal, out float4 tangent)
{
    // 원본 packed 속성과 정규화 weight를 읽는다.
    position = float4(asfloat(Source.Load3(address)), 1);
    normal = DecodeFrame(Source.Load(address + 12)).xyz;
    tangent = (Flags & 4) ? DecodeFrame(Source.Load(address + 16)) : float4(1, 0, 0, 1);
    uint4 packed = Source.Load4(skinAddress);
    uint4 indices = uint4(packed.x & 65535, packed.x >> 16, packed.y & 65535, packed.y >> 16);
    float4 weights = float4(packed.z & 65535, packed.z >> 16, packed.w & 65535, packed.w >> 16);
    weights /= max(dot(weights, 1), 1);

    // 동일 몸체의 Frozen 팔레트를 네 영향 모두에 적용한다.
    float4x4 skin = Joints[indices.x].PosMatrix * weights.x + Joints[indices.y].PosMatrix * weights.y
        + Joints[indices.z].PosMatrix * weights.z + Joints[indices.w].PosMatrix * weights.w;
    float4x3 skinNormal = Joints[indices.x].NrmMatrix * weights.x + Joints[indices.y].NrmMatrix * weights.y
        + Joints[indices.z].NrmMatrix * weights.z + Joints[indices.w].NrmMatrix * weights.w;
    position = mul(skin, position);
    normal = mul(skinNormal, normal).xyz;
    tangent.xyz = mul(skinNormal, tangent.xyz).xyz;
}

// 한 끝점의 원래 weight로 만든 법선 행렬을 방향 v에 적용한다.
float3 SkinDirection(uint skinAddress, float3 v)
{
    // 정규화 weight와 팔레트 인덱스를 읽는다.
    uint4 packed = Source.Load4(skinAddress);
    uint4 indices = uint4(packed.x & 65535, packed.x >> 16, packed.y & 65535, packed.y >> 16);
    float4 weights = float4(packed.z & 65535, packed.z >> 16, packed.w & 65535, packed.w >> 16);
    weights /= max(dot(weights, 1), 1);

    // 네 영향의 법선 행렬을 섞어 방향을 변환한다.
    float4x3 skinNormal = Joints[indices.x].NrmMatrix * weights.x + Joints[indices.y].NrmMatrix * weights.y
        + Joints[indices.z].NrmMatrix * weights.z + Joints[indices.w].NrmMatrix * weights.w;
    return mul(skinNormal, v).xyz;
}

// 단위 벡터 a를 b로 옮기는 최단 회전을 v에 적용한다(거의 반대 방향이면 v를 그대로 둔다).
float3 RotateArc(float3 a, float3 b, float3 v)
{
    // 회전축과 코사인으로 Rodrigues 식을 계산한다.
    float3 axis = cross(a, b);
    float c = dot(a, b);
    if (c < -0.999)
        return v;
    return v + cross(axis, v) + cross(axis, cross(axis, v)) / (1 + c);
}

// static·skeletal을 같은 posed local 형식으로 변환하며 원본 정점 번호를 유지한다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 원본 정점 범위와 packed 입력을 해석한다.
    uint vertex = id.x + id.y * (65535 * 64);
    if (vertex >= VertexCount)
        return;
    uint address = SourceOffset + vertex * SourceStride;
    float4 position = float4(asfloat(Source.Load3(address)), 1);
    float3 normal = DecodeFrame(Source.Load(address + 12)).xyz;
    float4 tangent = (Flags & 4) ? DecodeFrame(Source.Load(address + 16)) : float4(1, 0, 0, 1);
    if (Flags & 0x100)
    {
        // 일반 복사 정점과 실제 rim/cap은 기존 단일 skin 경로를 유지한다.
        SkinEndpoint(address, address + SourceStride - 16, position, normal, tangent);
        if (Flags & ExtFlag)
        {
            uint interpolation = Source.Load(address + SourceStride - ExtInternal);
            if (interpolation != 0)
            {
                // 내부 교점(1)·rim(2)·cap(3)은 두 끝점 skin 결과를 보간한다.
                float4 p0, p1, t0, t1;
                float3 n0, n1;
                SkinEndpoint(address + SourceStride - ExtEndpoint0, address + SourceStride - ExtEndpoint0 + EndpointSkin, p0, n0, t0);
                SkinEndpoint(address + SourceStride - ExtEndpoint1, address + SourceStride - ExtEndpoint1 + EndpointSkin, p1, n1, t1);
                float t = asfloat(Source.Load(address + SourceStride - ExtT));
                position = lerp(p0, p1, t);

                // rim·교점(1·2)은 끝점 방향을 보간하고, cap(3)은 끝점이 받는 현재 스키닝으로 K 법선을 다시 만들어 보간한다.
                if (interpolation != 3)
                {
                    normal = normalize(lerp(n0, n1, t));
                    tangent.xyz = normalize(lerp(t0.xyz, t1.xyz, t));
                }
                else
                {
                    // 끝점 탄젠트 칸에는 K 법선을 끝점 스키닝 이전으로 되돌린 방향이 들어 있고, 0이면 없음이라 주 레코드(K) 방향을 유지한다.
                    uint cut0 = Source.Load(address + SourceStride - ExtEndpoint0 + EndpointTangent);
                    uint cut1 = Source.Load(address + SourceStride - ExtEndpoint1 + EndpointTangent);
                    if (cut0 != 0 && cut1 != 0)
                    {
                        // 두 끝점의 현재 스키닝을 되돌린 방향에 적용하고 t로 보간해 정규화한다.
                        float3 s0 = SkinDirection(address + SourceStride - ExtEndpoint0 + EndpointSkin, DecodeFrame(cut0).xyz);
                        float3 s1 = SkinDirection(address + SourceStride - ExtEndpoint1 + EndpointSkin, DecodeFrame(cut1).xyz);
                        float3 cn = lerp(normalize(s0), normalize(s1), t);

                        // 퇴화하지 않으면 K 법선이 새 법선으로 가는 최단 회전을 탄젠트에 적용한다.
                        if (dot(cn, cn) > 1e-8)
                        {
                            cn = normalize(cn);
                            tangent.xyz = normalize(RotateArc(normalize(normal), cn, tangent.xyz));
                            normal = cn;
                        }
                    }
                }
            }
        }
    }

    // component-local 변환을 한 번 적용해 공통 RAW40 출력을 기록한다.
    uint outputAddress = vertex * 40;
    Result.Store3(outputAddress, asuint(mul(NodeMatrix, position).xyz));
    Result.Store3(outputAddress + 12, asuint(mul(NodeNormalMatrix, normal)));
    Result.Store4(outputAddress + 24, asuint(float4(mul(NodeNormalMatrix, tangent.xyz), tangent.w)));
}
