cbuffer Constants : register(b0)
{
    float4 Plane;
    float4x4 LocalToPlane;
    float2 HalfSize;
    float Epsilon;
    uint NodeCount;
    uint VertexOffset, VertexStride, IndexOffset, Index32;
    uint StartIndex;
    int BaseVertex;
    uint Triangles, PoseOffset, StartJoint;
    uint VertexCount, JointCount;
};
ByteAddressBuffer Source : register(t0);
ByteAddressBuffer Pose : register(t1);
StructuredBuffer<uint> JointNodes : register(t2);
RWByteAddressBuffer Votes : register(u0); // NodeCount * uint64, followed by crossing count.
RWByteAddressBuffer Result : register(u1);

// 원본 draw의 R16/R32 인덱스를 꼭짓점 번호로 변환한다.
uint VertexIndex(uint index)
{
    // R16은 정렬된 DWORD에서 해당 절반을 고른다.
    uint address = IndexOffset + index * (Index32 ? 4 : 2);
    uint packed = Source.Load(address & ~3u);
    uint vertex = Index32 ? packed : (packed >> ((address & 2) * 8)) & 65535;
    if (BaseVertex > 0 && vertex > 0xffffffffu - uint(BaseVertex))
        return 0xffffffffu;
    return vertex + BaseVertex;
}

// CPU 기준과 같은 XZ 구간 검사로 유한 사각형 겹침을 판정한다.
bool Overlaps(float3 start, float3 end)
{
    // 교차 선분을 평면 좌표로 옮기고 공면 여부를 검사한다.
    float3 a = mul(LocalToPlane, float4(start, 1)).xyz;
    float3 b = mul(LocalToPlane, float4(end, 1)).xyz;
    if (!all(isfinite(a)) || !all(isfinite(b)) || abs(a.y) > 1e-5 || abs(b.y) > 1e-5)
        return false;

    // 평행한 변과 일반적인 진입·이탈 구간을 동일하게 처리한다.
    float t0 = 0;
    float t1 = 1;
    for (uint axis = 0; axis < 2; ++axis)
    {
        float position = a[axis * 2];
        float delta = b[axis * 2] - position;
        if (delta == 0)
        {
            if (abs(position) > HalfSize[axis])
                return false;
            continue;
        }
        float enter = (-HalfSize[axis] - position) / delta;
        float leave = (HalfSize[axis] - position) / delta;
        t0 = max(t0, min(enter, leave));
        t1 = min(t1, max(enter, leave));
        if (t0 > t1)
            return false;
    }
    return true;
}

// 교차 삼각형의 정규화 weight를 NodeId별 64비트 고정소수 투표로 합친다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // draw offset과 posed RAW40 구간으로 세 꼭짓점을 읽는다.
    uint triangleId = id.x + id.y * (65535 * 64);
    if (triangleId >= Triangles)
        return;
    uint indices[3];
    float3 points[3];
    float distances[3];
    for (uint i = 0; i < 3; ++i)
    {
        indices[i] = VertexIndex(StartIndex + triangleId * 3 + i);
        if (indices[i] >= VertexCount)
            return;
        points[i] = asfloat(Pose.Load3(PoseOffset + indices[i] * 40));
        float distance = dot(Plane, float4(points[i], 1));
        distances[i] = abs(distance) <= Epsilon ? 0 : distance;
        if (distances[i] == 0)
            points[i] -= Plane.xyz * distance;
    }
    if (min(distances[0], min(distances[1], distances[2])) >= -Epsilon
        || max(distances[0], max(distances[1], distances[2])) <= Epsilon)
        return;

    // 평면 위 점과 반대 부호 변의 교점 두 개를 만든다.
    float3 hits[3];
    uint count = 0;
    for (uint edge = 0; edge < 3; ++edge)
    {
        uint next = (edge + 1) % 3;
        if (distances[edge] == 0)
            hits[count++] = points[edge];
        if (distances[edge] * distances[next] < 0)
            hits[count++] = lerp(points[edge], points[next], distances[edge] / (distances[edge] - distances[next]));
    }
    if (count != 2 || !Overlaps(hits[0], hits[1]))
        return;
    Votes.InterlockedAdd(NodeCount * 8, 1);

    // PreparePoseCS와 같은 packed decode·정규화 뒤 CPU 기준의 65535 배율로 반올림한다.
    for (uint corner = 0; corner < 3; ++corner)
    {
        uint4 packed = Source.Load4(VertexOffset + indices[corner] * VertexStride + VertexStride - 16);
        uint4 palette = uint4(packed.x & 65535, packed.x >> 16, packed.y & 65535, packed.y >> 16);
        float4 weights = float4(packed.z & 65535, packed.z >> 16, packed.w & 65535, packed.w >> 16);
        weights /= max(dot(weights, 1), 1);
        for (uint slot = 0; slot < 4; ++slot)
        {
            if (weights[slot] == 0 || palette[slot] >= JointCount)
                continue;
            uint node = JointNodes[StartJoint + palette[slot]];
            if (node >= NodeCount)
                continue;
            uint weight = uint(floor(weights[slot] * 65535 + 0.5));
            uint previous;
            Votes.InterlockedAdd(node * 8, weight, previous);
            if (previous > 0xffffffffu - weight)
                Votes.InterlockedAdd(node * 8 + 4, 1);
        }
    }
}
