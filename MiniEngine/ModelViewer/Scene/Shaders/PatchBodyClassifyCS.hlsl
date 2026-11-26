cbuffer Parameters : register(b0)
{
    uint FirstTriangle, TriangleCount, PatchTriangles, Side;
    uint DefaultBody, CapLoopCount;
    uint BodyCount, UseGpu;
#ifdef GPU_CLASSIFY
    uint OverrideCount, Verify, Padding0, Padding1;
#endif
};
ByteAddressBuffer Provenance : register(t0);
ByteAddressBuffer TriangleSideBody : register(t1);
ByteAddressBuffer CapLoopBodies : register(t2); // leader별 uint2, UINT_MAX=미해결, UINT_MAX-1=drop
#ifdef GPU_CLASSIFY
ByteAddressBuffer TriangleSideIsland : register(t3);
ByteAddressBuffer IslandOverrides : register(t4); // {island, body} 8B 목록, 최대 64개
ByteAddressBuffer CpuTriangleSideBody : register(t5); // verify에서만 바인딩
#endif
RWByteAddressBuffer TriangleBodies : register(u0);
RWByteAddressBuffer Counters : register(u1); // 기존 10개 진단 뒤 body별 표면 수, cap 수

// 출력 출처를 검사하고 CPU 표 또는 보정된 GPU 섬 표의 몸체를 기록한다.
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    // 2차원 dispatch를 펼치고 section의 빈 capacity 구간은 건너뛴다.
    uint local = (group.y * 65535 + group.x) * 64 + lane;
    if (local >= TriangleCount)
        return;
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

        // drop 전용 값과 유효 몸체를 구별하고 범위 밖 cap은 압축 실패로 전달한다.
        if (capBody < BodyCount)
        {
            Counters.InterlockedAdd((10 + BodyCount + capBody) * 4, 1);
        }
        else if (capBody != 0xfffffffe)
        {
            capBody = 0xffffffff;
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
    if (mismatch)
        Counters.InterlockedAdd(20, 1);
    uint body = 0xffffffff;
    if (!mismatch && id0 < PatchTriangles)
    {
        body = TriangleSideBody.Load((id0 * 2 + Side) * 4);
#ifdef GPU_CLASSIFY
        if (UseGpu)
        {
            // GPU 섬 번호로 CPU cap 보정이 변경한 섬의 몸체만 덮어쓴다.
            uint island = TriangleSideIsland.Load((id0 * 2 + Side) * 4);
            for (uint i = 0; i < OverrideCount; ++i)
                if (IslandOverrides.Load(i * 8) == island)
                    body = IslandOverrides.Load(i * 8 + 4);
        }
#endif
    }

#ifdef GPU_CLASSIFY
    // 같은 출력 삼각형에서 CPU 표 기준을 계산해 표면 수와 개별 몸체를 검증한다.
    if (Verify)
    {
        uint cpu = 0xffffffff;
        if (!mismatch && id0 < PatchTriangles)
            cpu = CpuTriangleSideBody.Load((id0 * 2 + Side) * 4);
        if (cpu < BodyCount)
            Counters.InterlockedAdd((10 + BodyCount * 2 + cpu) * 4, 1);
        if (cpu != body)
            Counters.InterlockedAdd((10 + BodyCount * 3) * 4, 1);
    }
#endif
    TriangleBodies.Store(outputTriangle * 4, body);

    // invalid는 숨기거나 대체하지 않고 남기며 유효한 표면만 몸체·재배치 수에 포함한다.
    if (body >= BodyCount)
        Counters.InterlockedAdd(16, 1);
    else
    {
        Counters.InterlockedAdd((10 + body) * 4, 1);
        if (body < 2)
            Counters.InterlockedAdd(8 + body * 4, 1);
        if (body != DefaultBody)
            Counters.InterlockedAdd(24, 1);
    }
}
