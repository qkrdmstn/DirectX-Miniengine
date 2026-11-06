#include "CutCommon.hlsli"

// (스레드 = 출력 정점)
groupshared uint3 Minimum[64], Maximum[64]; // AABB 축소용 (스레드마다 한 칸)
groupshared uint DopMin[9][64];             // DOP축 9개 x 스레드 64개
groupshared uint DopMax[9][64];             
groupshared uint DopCount[64];              // 기여한 정점 수

// 18-DOP의 9개 축으로 min, max 값을가지기 때문에 18개 평면을 생성한다.
static const float3 DopAxes[9] =
{
    float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1),
    float3(0, .70710678, .70710678), float3(0, .70710678, -.70710678),
    float3(.70710678, 0, .70710678), float3(.70710678, 0, -.70710678),
    float3(.70710678, .70710678, 0), float3(.70710678, -.70710678, 0)
};

// 정점 하나를 9축 투영을 그룹 안에서 축소한 뒤 면별 전역 극값과 기여 정점 수에 합친다.
void ReduceDop(uint side, bool valid, float3 p, uint tid)
{
    
    // 현재 정점을 9개의 축에 투영해서 내 레지스터에 저장
    // 현재 정점에서 유효하지 않은 값이 계산될 경우 모든 축에 대해 적용 X
    bool contributes = valid;
    float projection[9];
    [unroll]
    for (uint k = 0; k < 9; k++)
    {
        projection[k] = dot(p, DopAxes[k]);                     // 정점을 축 방향으로 투영해서 측정한 거리
        contributes = contributes && isfinite(projection[k]);   // NaN, 무한대 값이 한 번이라도 계산되는지 확인 => 유효하지 않으면 기여 X
    }

    // groupshared에 투영한 값을 저장
    // 유효하지 않으면 영향을 미치지 않는 값을 저장 (0xffffffff, 0)
    [unroll]
    for (uint a = 0; a < 9; a++)
    {
        DopMin[a][tid] = contributes ? Ordered(projection[a]) : 0xffffffff; 
        DopMax[a][tid] = contributes ? Ordered(projection[a]) : 0;
    }
    DopCount[tid] = contributes ? 1 : 0;
    
    // 그룹 내의 스레드가 여기까지 도달하기를 기다림.
    GroupMemoryBarrierWithGroupSync();

    // 매 단계 절반씩 축소하면서 각 축에 대응하는 최소, 최대값을 계산한다.
    // 최종 결과는 0 번째 인덱스에 저장
    for (uint offset = 32; offset > 0; offset /= 2)
    {
        if (tid < offset)
        {
            [unroll]
            for (uint b = 0; b < 9; b++)
            {
                DopMin[b][tid] = min(DopMin[b][tid], DopMin[b][tid + offset]);
                DopMax[b][tid] = max(DopMax[b][tid], DopMax[b][tid + offset]);
            }
            DopCount[tid] += DopCount[tid + offset];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    // 한 스레드 그룹의 결과를 모든 그룹이 공유하는 Meta에서 합친다.
    if (tid == 0 && DopCount[0] != 0) // 유효한 정점이 있는 스레드 그룹에서 대표 스레드 하나만 Meta를 업데이트한다.
    {
        uint ignored;
        uint base = (20 + 2 * Sections + side * 19) * 4;
        [unroll]
        for (uint c = 0; c < 9; c++)
        {
            Meta.InterlockedMin(base + c * 4, DopMin[c][0], ignored);
            Meta.InterlockedMax(base + 36 + c * 4, DopMax[c][0], ignored);
        }
        Meta.InterlockedAdd(base + 72, DopCount[0], ignored);
    }
    GroupMemoryBarrierWithGroupSync();
}

// 정점마다 전역 atomic을 수행하지 않고 그룹별 축소 후, AABB를 합친다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint tid : SV_GroupIndex)
{
    // 전체 정점 번호에서 소속 section을 찾는다.
    uint vertex = id.x, section = 0;
    while (section + 1 < Sections && vertex >= Inputs[section + 1].VertexStart)
        section++;
    Section s = Inputs[section];

    // 양쪽 Data의 component-local 위치로 AABB와 DOP를 계산한다.
    [unroll]
    for (uint side = 0; side < 2; side++)
    {
        // 실제 정점인지 확인하고 출력 버퍼 A/B에서 위치를 읽는다.
        uint count = Meta.Load(80 + (side * Sections + section) * 4);
        bool valid = vertex < VertexCapacity && vertex >= s.VertexStart && vertex - s.VertexStart < min(count, s.VertexCapacity);
        float3 p = 0;
        if (valid)
        {
            uint address = s.VertexStart * 28 + (vertex - s.VertexStart) * 28;
            p = side == 0 ? asfloat(DataA.Load3(address)) : asfloat(DataB.Load3(address));
        }

        // Todo: 절반씩 접어서 최소/최대값 업데이트하는 거 중복 코드임 함수화 해서 줄일 것.
        // 현재 스레드 그룹으로 지정된 정점들의 AABB를 계산
        uint3 ordered = uint3(Ordered(p.x), Ordered(p.y), Ordered(p.z));
        Minimum[tid] = valid ? ordered : uint3(0xffffffff, 0xffffffff, 0xffffffff);
        Maximum[tid] = valid ? ordered : uint3(0, 0, 0);
        GroupMemoryBarrierWithGroupSync();
        for (uint offset = 32; offset > 0; offset /= 2)
        {
            if (tid < offset)
            {
                Minimum[tid] = min(Minimum[tid], Minimum[tid + offset]);
                Maximum[tid] = max(Maximum[tid], Maximum[tid + offset]);
            }
            GroupMemoryBarrierWithGroupSync();
        }
        
        // 각 그룹에서 대표 스레드 하나만 이 작업을 수행
        if (tid == 0)
        {
            uint ignored;
            [unroll]
            for (uint k = 0; k < 3; k++)
            {
                Meta.InterlockedMin(32 + side * 24 + k * 4, Minimum[0][k], ignored);
                Meta.InterlockedMax(44 + side * 24 + k * 4, Maximum[0][k], ignored);
            }
        }
        GroupMemoryBarrierWithGroupSync();

        // 같은 위치를 18-DOP 축에 투영한다.
        ReduceDop(side, valid, p, tid);
    }
}
