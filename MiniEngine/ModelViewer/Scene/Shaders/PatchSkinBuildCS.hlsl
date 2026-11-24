cbuffer Parameters : register(b0)
{
    float4x4 InverseNode;
    uint FirstVertex, VertexCount, Flags, Stride;
    uint DrawCount, RigidJoint, PatchTriangles, Follow;
    uint LengthEntries, BoundaryCount, Pad0, Pad1;
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
struct LengthEntry
{
    float Ratio;
    uint Descendant;
    int Fallback;
    uint Pad;
};
StructuredBuffer<LengthEntry> LengthTable : register(t6);
StructuredBuffer<uint> BoundaryFlags : register(t7);
RWByteAddressBuffer LengthCounters : register(u1);
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
// 끝점 재분배 계수의 시작 바이트이다(주 레코드 계수 32B 뒤).
static const uint EndpointCounters = 32;

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

// 법선 행렬 합의 새 역행렬과 옛 행렬을 이어 bind 방향 하나를 다시 계산해 압축한다(w 두 비트는 유지하고, 퇴화하면 원래 값을 돌려준다).
uint RemapFrame(uint packed, float3x3 inverseNew, float3x3 oldNormal)
{
    // 옛 법선 행렬로 절단 시점 방향을 만든 뒤 새 법선 행렬의 역으로 bind 방향을 구한다.
    float4 frame = Frame(packed);
    float3 direction = mul(inverseNew, mul(oldNormal, frame.xyz));
    if (!(dot(direction, direction) > 1e-20))
    {
        return packed;
    }

    // 정규화해 기존 UNORM 10-10-10-2 형식으로 다시 압축하고 w 비트는 원래 값을 쓴다.
    uint3 value = uint3(round(saturate(normalize(direction) * .5 + .5) * 1023));
    return value.x | (value.y << 10) | (value.z << 20) | (packed & 0xc0000000);
}

// 복사 정점의 u16 skin 입력(joints 전체 팔레트 인덱스, weights u16)을 남은 길이 비율로 재분배하고 bind 위치·법선·탄젠트를 다시 계산한다.
// 재분배 대상 weight가 없거나 폴백 본 없음, 비가역이면 입력을 그대로 두고 false를 돌려주며, 법선 행렬만 비가역이면 법선·탄젠트만 유지하고 계수한다.
// base는 계수 버퍼의 시작 바이트이다(0은 주 레코드, 32는 끝점 레코드이며 각 묶음의 24는 법선 비가역, 28은 법선 갱신 수이다).
bool Reweight(inout float3 bind, inout uint4 joints, inout uint4 weights16, inout uint normal, inout uint tangent, bool hasTangent, uint base)
{
    // weight가 있는 슬롯 중 비율 1 미만이 없으면 연산 없이 원래 레코드를 유지한다(weight 0 슬롯의 비율은 무시한다).
    bool affected = false;
    for (uint slot = 0; slot < 4; ++slot)
    {
        if (weights16[slot] > 0 && LengthTable[joints[slot]].Ratio < 1.0)
        {
            affected = true;
        }
    }
    if (!affected)
    {
        LengthCounters.InterlockedAdd(base + 20, 1);
        return false;
    }

    // 정규화 weight와 비율의 가중합을 구한다.
    float4 weights = float4(weights16);
    weights /= max(dot(weights, 1), 1);
    float4 ratios = float4(LengthTable[joints.x].Ratio, LengthTable[joints.y].Ratio, LengthTable[joints.z].Ratio, LengthTable[joints.w].Ratio);
    float4 scaled = weights * ratios;
    float sum = dot(scaled, 1);
    uint4 newJoints = joints;
    uint4 quantized;

    // 합이 너무 작으면 최대 weight joint의 P_i 하나에 1을 준다(없으면 원래 레코드를 유지한다).
    if (sum < 1e-3)
    {
        uint best = 0;
        for (uint k = 1; k < 4; ++k)
        {
            if (weights[k] > weights[best])
            {
                best = k;
            }
        }
        int fallback = LengthTable[joints[best]].Fallback;
        if (fallback < 0)
        {
            LengthCounters.InterlockedAdd(base + 12, 1);
            return false;
        }
        newJoints[best] = uint(fallback);
        quantized = uint4(0, 0, 0, 0);
        quantized[best] = 65535;
        LengthCounters.InterlockedAdd(base + 8, 1);
    }
    else
    {
        // 재정규화한 weight를 반올림하고 나머지는 가장 큰 항목에 더해 합을 65535로 맞춘다.
        scaled /= sum;
        quantized = uint4(round(scaled * 65535));
        uint largest = 0;
        for (uint k = 1; k < 4; ++k)
        {
            if (scaled[k] > scaled[largest])
            {
                largest = k;
            }
        }
        quantized[largest] = uint(int(quantized[largest]) + 65535 - int(quantized.x + quantized.y + quantized.z + quantized.w));
    }

    // 원래와 새 weight로 절단 시점 skin 행렬 M, M'를 만든다.
    uint4 oldJoints = joints;
    float4 before = weights;
    float4 after = float4(quantized) / 65535.0;
    float4x4 oldSkin = JointsAtCut[joints.x].PosMatrix * before.x + JointsAtCut[joints.y].PosMatrix * before.y
        + JointsAtCut[joints.z].PosMatrix * before.z + JointsAtCut[joints.w].PosMatrix * before.w;
    float4x4 newSkin = JointsAtCut[newJoints.x].PosMatrix * after.x + JointsAtCut[newJoints.y].PosMatrix * after.y
        + JointsAtCut[newJoints.z].PosMatrix * after.z + JointsAtCut[newJoints.w].PosMatrix * after.w;

    // M' 3x3의 행렬식이 M에 비해 거의 0이면 원래 레코드를 유지한다.
    float3x3 newLinear = (float3x3)newSkin;
    float3x3 cofactor = float3x3(cross(newLinear[1], newLinear[2]), cross(newLinear[2], newLinear[0]), cross(newLinear[0], newLinear[1]));
    float determinant = dot(newLinear[0], cofactor[0]);
    float3x3 oldLinear = (float3x3)oldSkin;
    float oldDeterminant = dot(oldLinear[0], cross(oldLinear[1], oldLinear[2]));
    if (!(abs(determinant) > 1e-4 * abs(oldDeterminant)) || abs(determinant) < 1e-20)
    {
        LengthCounters.InterlockedAdd(base + 16, 1);
        return false;
    }

    // bind' = inv(M')·M·bind 를 3x3 역(cofactor/det)과 이동으로 계산한다.
    float3 skinned = mul(oldSkin, float4(bind, 1)).xyz;
    float3 translation = float3(newSkin[0][3], newSkin[1][3], newSkin[2][3]);
    bind = mul(transpose(cofactor), skinned - translation) / determinant;
    joints = newJoints;
    weights16 = quantized;

    // 법선 행렬의 합 A, A'를 만든다(PreparePoseCS와 같은 Σw·NrmMatrix이며 M'의 역전치가 아니다).
    float4x3 oldSum = JointsAtCut[oldJoints.x].NrmMatrix * before.x + JointsAtCut[oldJoints.y].NrmMatrix * before.y
        + JointsAtCut[oldJoints.z].NrmMatrix * before.z + JointsAtCut[oldJoints.w].NrmMatrix * before.w;
    float4x3 newSum = JointsAtCut[newJoints.x].NrmMatrix * after.x + JointsAtCut[newJoints.y].NrmMatrix * after.y
        + JointsAtCut[newJoints.z].NrmMatrix * after.z + JointsAtCut[newJoints.w].NrmMatrix * after.w;
    float3x3 oldNormal = (float3x3)oldSum;
    float3x3 newNormal = (float3x3)newSum;

    // A'의 역(cofactor/det)이 퇴화하면 법선·탄젠트는 원래 값을 유지하고 계수한다.
    float3x3 normalCofactor = float3x3(cross(newNormal[1], newNormal[2]), cross(newNormal[2], newNormal[0]), cross(newNormal[0], newNormal[1]));
    float normalDeterminant = dot(newNormal[0], normalCofactor[0]);
    float oldNormalDeterminant = dot(oldNormal[0], cross(oldNormal[1], oldNormal[2]));
    if (!(abs(normalDeterminant) > 1e-4 * abs(oldNormalDeterminant)) || abs(normalDeterminant) < 1e-20)
    {
        LengthCounters.InterlockedAdd(base + 24, 1);
    }
    else
    {
        // 법선과 탄젠트 xyz를 같은 식으로 다시 계산한다.
        float3x3 inverseNormal = transpose(normalCofactor) / normalDeterminant;
        uint originalNormal = normal;
        normal = RemapFrame(normal, inverseNormal, oldNormal);
        if (normal != originalNormal)
        {
            LengthCounters.InterlockedAdd(base + 28, 1);
        }
        if (hasTangent)
        {
            tangent = RemapFrame(tangent, inverseNormal, oldNormal);
        }
    }
    LengthCounters.InterlockedAdd(base + 0, 1);
    return true;
}

// 끝점은 원본 속성과 전체 팔레트 인덱스를 별도로 보관한다. cap 끝점은 탄젠트 칸에 절단 시점 법선(K 프레임)을 넣는다.
// 재분배가 켜져 있으면 경계가 아닌 끝점의 weight와 bind 위치·법선·탄젠트를 복사 정점과 같은 규칙으로 다시 계산한다(cap의 절단 시점 법선은 원본 weight 기준 그대로다).
void StoreEndpoint(uint output, uint vertex, bool cap)
{
    // 두 끝점의 weight를 합치지 않고 각각의 원본 속성과 skin 입력을 읽는다.
    DrawInput draw;
    uint address = SourceAddress(vertex, draw);
    float3 bind = asfloat(Source.Load3(address));
    uint normal = Source.Load(address + 12);
    uint tangent = (draw.SourceFlags & 4) ? Source.Load(address + 16) : 0xe00803ff;
    uint4 skin = Source.Load4(address + draw.SourceStride - 16);
    uint4 joints = uint4(skin.x & 65535, skin.x >> 16, skin.y & 65535, skin.y >> 16) + draw.StartJoint;
    uint4 weights16 = uint4(skin.z & 65535, skin.z >> 16, skin.w & 65535, skin.w >> 16);

    // 경계 끝점은 원래 레코드를 유지하고, 그 밖의 끝점은 재분배한다(cap은 탄젠트 칸을 따로 쓰므로 탄젠트를 바꾸지 않는다).
    if (LengthEntries)
    {
        if (vertex >= BoundaryCount || BoundaryFlags[vertex])
        {
            LengthCounters.InterlockedAdd(EndpointCounters + 4, 1);
        }
        else
        {
            Reweight(bind, joints, weights16, normal, tangent, !cap && (draw.SourceFlags & 4) != 0, EndpointCounters);
        }
    }

    // 위치·법선·탄젠트 칸을 기록하고, cap은 탄젠트 칸에 절단 시점 법선을 넣어 PreparePoseCS가 끝점의 절단 시점 → 현재 법선 회전을 구하게 한다.
    Result.Store3(output, asuint(bind));
    Result.Store(output + 12, normal);
    Result.Store(output + EndpointTangent, cap ? CutTimeNormal(address, draw) : tangent);

    // 전체 팔레트를 가리키는 joints와 weights를 기록한다.
    Result.Store4(output + EndpointSkin, uint4(joints.x | (joints.y << 16), joints.z | (joints.w << 16),
        weights16.x | (weights16.y << 16), weights16.z | (weights16.w << 16)));
}

// 복사 정점은 원본 bind로 복원하며(재분배가 켜진 A mesh의 비경계 복사 정점은 weight와 bind 위치·법선·탄젠트를 다시 계산한다) 내부 교점(1)과, 조각 플래그가 켜진 실제 분리 rim(2)·cap(3)에는 두 끝점의 원본 skin 입력을 덧붙인다.
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
    bool reweighted = false;
    uint4 newJoints = uint4(0, 0, 0, 0);
    uint copiedNormal = 0;
    uint copiedTangent = 0;

    // 복사된 패치 정점을 section/draw/sourceTriangle과 corner로 원본 VB에 대응시킨다.
    if (copied)
    {
        DrawInput draw;
        address = SourceAddress(origin.z, draw);
        sourceFlags = draw.SourceFlags;
        startJoint = draw.StartJoint;
        skin = Source.Load4(address + draw.SourceStride - 16);
        float3 bind = asfloat(Source.Load3(address));
        copiedNormal = Source.Load(address + 12);
        copiedTangent = (sourceFlags & 4) ? Source.Load(address + 16) : 0xe00803ff;

        // 남은 길이 재분배가 켜져 있으면 경계가 아닌 정점만 weight와 bind 위치를 바꾼다(경계 번호를 못 읽으면 경계로 본다).
        if (LengthEntries)
        {
            if (origin.z >= BoundaryCount || BoundaryFlags[origin.z])
            {
                LengthCounters.InterlockedAdd(4, 1);
            }
            else
            {
                uint4 full = uint4(skin.x & 65535, skin.x >> 16, skin.y & 65535, skin.y >> 16) + startJoint;
                uint4 packed = uint4(skin.z & 65535, skin.z >> 16, skin.w & 65535, skin.w >> 16);
                reweighted = Reweight(bind, full, packed, copiedNormal, copiedTangent, (sourceFlags & 4) != 0, 0);
                if (reweighted)
                {
                    newJoints = full;
                    skin.zw = uint2(packed.x | (packed.y << 16), packed.z | (packed.w << 16));
                }
            }
        }
        Result.Store3(output, asuint(bind));
        Result.Store(output + 12, copiedNormal);
    }
    else
    {
        Result.Store3(output, asuint(mul(InverseNode, float4(asfloat(Data.Load3(address)), 1)).xyz));
        Result.Store(output + 12, RotateFrame(Data.Load(address + 12)));
    }

    // section별 flags의 합집합을 사용하고 없는 속성에는 기본값을 넣는다.
    uint tangent = (sourceFlags & 4) ? (copied ? copiedTangent : RotateFrame(Data.Load(address + 16))) : 0xe00803ff;
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
    if (reweighted)
    {
        joints = newJoints;
    }
    Result.Store4(output + Stride - 16, uint4(joints.x | (joints.y << 16), joints.z | (joints.w << 16), skin.zw));
}
