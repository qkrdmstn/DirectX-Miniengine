cbuffer Parameters : register(b0)
{
    uint PoseOffset, VertexOffset, DepthOffset, VertexCount;
    uint DepthStride;
};
ByteAddressBuffer Pose : register(t0);
RWByteAddressBuffer Data : register(u0);

// cutter와 같은 길이 검사로 프레임 방향을 정규화한다.
float3 SafeNormal(float3 value)
{
    // 길이가 없는 프레임은 cutter의 기본 축을 사용한다.
    return dot(value, value) > 1e-20 ? normalize(value) : float3(0, 1, 0);
}

// cutter와 같은 UNORM 반올림으로 법선과 탄젠트를 압축한다.
uint PackFrame(float4 value)
{
    // 각 성분을 10/10/10/2 비트 범위로 양자화한다.
    uint4 packed = uint4(round(saturate(value * 0.5 + 0.5) * float4(1023, 1023, 1023, 3)));
    return packed.x | (packed.y << 10) | (packed.z << 20) | (packed.w << 30);
}

// 현재 RAW40 속성만 기존 Data 복사본에 덮어 UV와 인덱스를 보존한다.
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    // 큰 section도 2차원 dispatch에서 선형 정점 번호로 처리한다.
    uint vertex = (group.y * 65535 + group.x) * 64 + lane;
    if (vertex >= VertexCount)
        return;

    // 표면과 깊이 VB에 같은 위치를 기록하고 프레임 속성만 재압축한다.
    uint source = PoseOffset + vertex * 40;
    uint target = VertexOffset + vertex * 28;
    uint3 position = Pose.Load3(source);
    Data.Store3(target, position);
    Data.Store(target + 12, PackFrame(float4(SafeNormal(asfloat(Pose.Load3(source + 12))), 1)));
    float4 tangent = asfloat(Pose.Load4(source + 24));
    tangent.xyz = SafeNormal(tangent.xyz);
    tangent.w = tangent.w < 0 ? -1 : 1;
    Data.Store(target + 16, PackFrame(tangent));
    Data.Store3(DepthOffset + vertex * DepthStride, position);
}
