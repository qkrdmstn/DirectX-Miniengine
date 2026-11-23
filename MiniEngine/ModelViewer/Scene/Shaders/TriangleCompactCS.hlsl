cbuffer Parameters : register(b0)
{
    uint Triangles, Draws, Mode, Padding;
};
struct DrawInput
{
    uint IbOffset, Index32, StartIndex, DrawTriangles;
    uint StateFirst, WorkFirst, Reserved0, Reserved1;
};
ByteAddressBuffer Source : register(t0);
StructuredBuffer<DrawInput> Inputs : register(t1);
StructuredBuffer<uint> State : register(t2);
RWByteAddressBuffer Scan : register(u0); // 삼각형별 블록 내부 inclusive prefix, 뒤쪽에 블록별 합계·offset
RWByteAddressBuffer Output : register(u1);
groupshared uint Prefix[64];

// 그룹의 남는 삼각형 수를 Hillis-Steele inclusive scan으로 계산한다.
uint ScanTotals(uint lane, uint value)
{
    // 범위 밖 lane도 0을 기록하고 모든 lane이 초기화를 마칠 때까지 기다린다.
    Prefix[lane] = value;
    GroupMemoryBarrierWithGroupSync();

    // 이전 단계 값을 모두 읽은 뒤 덮어쓰고 다음 단계 전에 동기화한다.
    [unroll] for (uint offset = 1; offset < 64; offset *= 2)
    {
        uint previous = 0;
        if (lane >= offset) previous = Prefix[lane - offset];
        GroupMemoryBarrierWithGroupSync();
        Prefix[lane] += previous;
        GroupMemoryBarrierWithGroupSync();
    }

    // 각 lane의 누적 합계를 반환한다.
    return Prefix[lane];
}

// 작업 번호가 속한 draw를 찾아 제거 비트가 없는 삼각형만 남는 것으로 본다.
bool Keeps(uint t, out DrawInput draw, out uint local)
{
    // draw는 작업 번호 순서로 놓여 있으므로 앞에서부터 찾는다.
    uint id = 0;
    while (id + 1 < Draws && t >= Inputs[id + 1].WorkFirst) id++;
    draw = Inputs[id];
    local = t - draw.WorkFirst;

    // 제거 상태 bit31이 없으면 남긴다.
    return !(State[draw.StateFirst + local] & 0x80000000u);
}

// 블록 prefix와 블록 offset으로 남는 삼각형의 원본 인덱스를 압축 위치에 쓴다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 블록 안 prefix와 블록 합계를 계산한다. 범위 밖 lane은 0으로 참여한다.
    uint t = id.x + id.y * (65535 * 64);
    uint lane = t % 64;
    if (Mode == 0)
    {
        uint kept = 0;
        DrawInput draw;
        uint local;
        if (t < Triangles) kept = Keeps(t, draw, local) ? 1 : 0;
        uint total = ScanTotals(lane, kept);
        if (t < Triangles) Scan.Store(t * 4, total);
        if (lane == 63 && t / 64 < (Triangles + 63) / 64) Scan.Store(Triangles * 4 + (t / 64) * 4, total);
        return;
    }

    // 한 그룹이 블록 합계를 scan해 각 블록의 exclusive offset으로 바꾼다.
    if (Mode == 1)
    {
        uint blocks = (Triangles + 63) / 64;
        uint count = (blocks + 63) / 64;
        uint begin = lane * count, end = min(begin + count, blocks);
        uint subtotal = 0;
        for (uint b = begin; b < end; b++) subtotal += Scan.Load(Triangles * 4 + b * 4);
        uint offset = ScanTotals(lane, subtotal) - subtotal;
        for (uint c = begin; c < end; c++)
        {
            uint address = Triangles * 4 + c * 4;
            uint selected = Scan.Load(address);
            Scan.Store(address, offset);
            offset += selected;
        }
        return;
    }

    // 남는 삼각형의 순위를 구해 원본 상대 인덱스 3개를 그대로 쓴다.
    if (t >= Triangles) return;
    DrawInput draw;
    uint local;
    if (!Keeps(t, draw, local)) return;
    uint rank = Scan.Load(t * 4) + Scan.Load(Triangles * 4 + (t / 64) * 4) - 1;
    [unroll] for (uint k = 0; k < 3; k++)
    {
        uint at = draw.StartIndex + local * 3 + k;
        uint word = Source.Load(draw.IbOffset + (draw.Index32 ? at * 4 : (at / 2) * 4));
        Output.Store((rank * 3 + k) * 4, draw.Index32 ? word : ((word >> ((at & 1) * 16)) & 65535));
    }
}
