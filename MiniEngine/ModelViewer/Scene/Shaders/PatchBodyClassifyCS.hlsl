cbuffer Parameters : register(b0)
{
    uint FirstTriangle, TriangleCount, PatchTriangles, Side;
    uint DefaultBody, CapLoopCount;
    uint2 Padding;
};
ByteAddressBuffer Provenance : register(t0);
ByteAddressBuffer TriangleSideBody : register(t1);
ByteAddressBuffer CapLoopBodies : register(t2); // leader별 uint2, UINT_MAX=미해결, UINT_MAX-1=drop
RWByteAddressBuffer TriangleBodies : register(u0);
RWByteAddressBuffer Counters : register(u1); // tris, cap, body0, body1, invalid, idMismatch, reassigned, dropped, capFallback, capInvalid

// 출력 삼각형의 세 출처를 검사하고 반공간별 아일랜드 몸체를 기록한다.
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    // 2차원 dispatch를 펼치고 section의 빈 capacity 구간은 건너뛴다.
    uint local = (group.y * 65535 + group.x) * 64 + lane;
    if (local >= TriangleCount) return;
    uint outputTriangle = FirstTriangle + local;
    uint address = outputTriangle * 3 * 32;
    Counters.InterlockedAdd(0, 1);

    // cap의 세 leader가 일치하면 loop 표로 귀속하고 drop을 별도로 기록한다.
    if (Provenance.Load(address) == 0xffffffff)
    {
        uint leader = Provenance.Load(address + 4);
        bool same = leader == Provenance.Load(address + 36) && leader == Provenance.Load(address + 68);
        uint capBody = 0xffffffff;
        if (same && leader < CapLoopCount)
            capBody = CapLoopBodies.Load((leader * 2 + Side) * 4);
        if (capBody == 0xffffffff && same)
        {
            capBody = DefaultBody;
            Counters.InterlockedAdd(32, 1);
        }
        if (!same)
        {
            Counters.InterlockedAdd(36, 1);
        }
        TriangleBodies.Store(outputTriangle * 4, capBody);
        Counters.InterlockedAdd(4, 1);
        if (capBody == 0xfffffffe)
        {
            Counters.InterlockedAdd(28, 1);
        }
        return;
    }

    // WriteVertex의 32B 레이아웃에서 Origin은 +0, Parameter는 +16, TriangleId는 +20이다.
    uint id0 = Provenance.Load(address + 20);
    uint id1 = Provenance.Load(address + 32 + 20);
    uint id2 = Provenance.Load(address + 64 + 20);
    bool mismatch = id0 != id1 || id0 != id2;
    if (mismatch) Counters.InterlockedAdd(20, 1);
    uint body = 0xffffffff;
    if (!mismatch && id0 < PatchTriangles)
        body = TriangleSideBody.Load((id0 * 2 + Side) * 4);
    TriangleBodies.Store(outputTriangle * 4, body);

    // invalid는 숨기거나 대체하지 않고 남기며 유효한 표면만 몸체·재배치 수에 포함한다.
    if (body == 0xffffffff)
        Counters.InterlockedAdd(16, 1);
    else
    {
        if (body < 2) Counters.InterlockedAdd(8 + body * 4, 1);
        if (body != DefaultBody) Counters.InterlockedAdd(24, 1);
    }
}
