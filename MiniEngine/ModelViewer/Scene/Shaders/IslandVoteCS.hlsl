cbuffer Parameters : register(b0)
{
    uint Capacity;
    uint Groups;
    uint Pass;
    uint Draws;
};
struct DrawInput
{
    uint VbOffset;
    uint Stride;
    uint IbOffset;
    uint Index32;
    uint StartIndex;
    uint BaseVertex;
    uint Triangles;
    uint PoseOffset;
    uint FirstTriangle;
    uint MaskOffset;
    uint Flags;
    uint SectionId;
    uint DrawId;
    uint JointCount;
    uint VertexCount;
    uint WordOffset;
};
ByteAddressBuffer Meta : register(t0);
ByteAddressBuffer Sources : register(t1);
StructuredBuffer<DrawInput> Inputs : register(t2);
StructuredBuffer<uint> Weights : register(t3);
StructuredBuffer<uint2> Islands : register(t4);
RWByteAddressBuffer Status : register(u0);
RWStructuredBuffer<uint> Output : register(u1);

// 수렴한 섬에 원시 bind weight를 정수 누적하고 몸체 표와 근접 판정을 만든다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 미수렴 요청은 번호와 투표 버퍼를 읽지 않는다.
    if (Status.Load(0) != 0)
        return;
    uint t = id.x;
    uint count = Meta.Load(0);
    uint stride = Groups + 5;
    uint table = Capacity * 2 * stride;
    uint tail = table + Capacity * 2;
    uint n = Status.Load(4);

    // 실제 활성 쌍을 먼저 세어 overflow 요청은 누적 전에 차단한다.
    if (Pass == 0)
    {
        if (t != 0)
            return;
        for (uint z = 0; z < n * stride; ++z)
            Output[z] = 0;
        uint active = 0;
        for (uint q = 0; q < count; ++q)
            active += (Islands[q].x != 0xffffffff) + (Islands[q].y != 0xffffffff);
        Output[tail + 2] = active >= 340000;
        return;
    }
    if (Output[tail + 2] != 0)
        return;

    // 존재하는 양쪽 side 각각에 세 꼭짓점 전체를 중복 투표한다.
    if (Pass == 1)
    {
        if (t >= count)
            return;
        uint3 origin = Sources.Load3(t * 12);
        uint draw = 0;
        while (draw < Draws && (Inputs[draw].SectionId != origin.x || Inputs[draw].DrawId != origin.y))
            ++draw;
        if (draw == Draws)
            return;
        for (uint side = 0; side < 2; ++side)
        {
            uint island = Islands[t][side];
            if (island == 0xffffffff)
                continue;
            uint row = island * stride;
            InterlockedAdd(Output[row + Groups], 1);
            InterlockedOr(Output[row + Groups + 1], side);
            for (uint k = 0; k < 3; ++k)
            {
                uint at = ((Inputs[draw].FirstTriangle + origin.z) * 3 + k) * 8;
                uint sum = Weights[at] + Weights[at + 1] + Weights[at + 2] + Weights[at + 3];
                for (uint j = 0; j < 4; ++j)
                {
                    uint w = Weights[at + j];
                    if (w != 0)
                        InterlockedAdd(Output[row + Weights[at + 4 + j]], w * 4096u / sum);
                }
            }
        }
        return;
    }

    // 작은 섬 표의 승자·실제 1·2위 차이·근접 여부를 순서대로 정한다.
    if (t != 0)
        return;
    for (uint i = 0; i < n; ++i)
    {
        uint row = i * stride;
        uint best = 0;
        uint runner = 0;
        uint body = 0;
        for (uint g = 0; g < Groups; ++g)
        {
            uint score = Output[row + g];
            if (score > best)
            {
                runner = best;
                best = score;
                body = g;
            }
            else if (score > runner)
                runner = score;
        }
        Output[row + Groups + 2] = best == runner ? 0 : body;
        Output[row + Groups + 3] = best - runner;
        Output[row + Groups + 4] = best - runner <= 12u * Output[row + Groups];
    }

    // 삼각형 몸체와 side 기본값을 만들며 기본값 동점은 작은 몸체를 고른다.
    for (uint q = 0; q < count; ++q)
        for (uint side = 0; side < 2; ++side)
            Output[table + q * 2 + side] = Islands[q][side] == 0xffffffff ? 0xffffffff : Output[Islands[q][side] * stride + Groups + 2];
    for (uint side = 0; side < 2; ++side)
    {
        uint best = 0;
        Output[tail + side] = 0;
        for (uint g = 0; g < Groups; ++g)
        {
            uint total = 0;
            for (uint i = 0; i < n; ++i)
                if (Output[i * stride + Groups + 1] == side && Output[i * stride + Groups + 2] == g)
                    total += Output[i * stride + Groups];
            if (total > best)
            {
                best = total;
                Output[tail + side] = g;
            }
        }
    }
}
