cbuffer Parameters : register(b0)
{
    uint Mode, BodyCount, Sections, Section;
    uint FirstTriangle, Triangles, VbOffset, DepthOffset;
    uint IbOffset, Stride, DepthStride, CapacityBytes;
    uint CapacityTriangles;
    uint3 Padding;
};
ByteAddressBuffer InputData : register(t0); // surface/depth VB 및 section 로컬 R32 IB
ByteAddressBuffer Provenance : register(t1); // 정점당 32B
ByteAddressBuffer Bodies : register(t2); // MI-3a의 globalVertex/3 주소
ByteAddressBuffer SectionLayouts : register(t3); // section당 uint2(surface stride, depth stride)
// 두 side의 대응 section은 같은 DWORD 정렬 stride를 사용하며 depth stride 0은 depth 없음이다.
// 호출자는 Data에 CapacityBytes, Provenance에 CapacityTriangles*96 바이트 이상을 할당한다.
RWByteAddressBuffer OutputData : register(u0);
RWByteAddressBuffer OutputProvenance : register(u1);
RWByteAddressBuffer Bins : register(u2); // body×section당 24B: count, firstTriangle, VB, depth, IB, cursor
RWByteAddressBuffer Status : register(u3); // bit0=capacity overflow, bit1=invalid body

// 작은 body×section 표를 prefix하여 surface 정점 번호와 출처 번호를 맞춘다.
void BuildOffsets()
{
    // 전체 용량을 검사하면서 모든 surface VB를 먼저 연속 배치한다.
    uint triangles = 0, bytes = 0, surfaceBytes = 0;
    for (uint bin = 0; bin < BodyCount * Sections; ++bin)
    {
        uint address = bin * 24;
        uint count = Bins.Load(address);
        uint2 layout = SectionLayouts.Load2((bin % Sections) * 8);
        uint triangleBytes = 3 * (layout.x + layout.y + 4);
        if (count > CapacityTriangles - triangles || count > (CapacityBytes - bytes) / triangleBytes)
        {
            Status.InterlockedOr(0, 1);
            return;
        }
        Bins.Store(address + 4, triangles);
        Bins.Store(address + 8, surfaceBytes);
        Bins.Store(address + 20, 0);
        // 실제 job은 작은 표 뒤에 section별 AABB를 함께 보관한다.
        if (Padding.x != 0)
        {
            uint bounds = (BodyCount * Sections + bin) * 24;
            Bins.Store3(bounds, uint3(0xffffffff, 0xffffffff, 0xffffffff));
            Bins.Store3(bounds + 12, uint3(0, 0, 0));
        }
        triangles += count;
        bytes += count * triangleBytes;
        surfaceBytes += count * 3 * layout.x;
    }

    // surface 영역 뒤에 각 bin의 depth와 IB를 배치하여 공통 정점 번호를 보존한다.
    bytes = surfaceBytes;
    for (uint bin = 0; bin < BodyCount * Sections; ++bin)
    {
        uint address = bin * 24;
        uint count = Bins.Load(address);
        uint depthStride = SectionLayouts.Load((bin % Sections) * 8 + 4);
        Bins.Store(address + 12, bytes);
        bytes += count * 3 * depthStride;
        Bins.Store(address + 16, bytes);
        bytes += count * 3 * 4;
    }
}

// 두 side를 같은 표에 누적한 뒤 몸체별 section으로 정점과 출처를 압축한다.
[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    // Mode=1은 Dispatch(1,1,1)로 호출하고 나머지는 section의 실제 삼각형만 순회한다.
    if (Mode == 1)
    {
        if (lane == 0) BuildOffsets();
        return;
    }
    uint localTriangle = (group.y * 65535 + group.x) * 64 + lane;
    if (localTriangle >= Triangles) return;
    uint body = Bodies.Load((FirstTriangle + localTriangle) * 4);
    if (body >= BodyCount)
    {
        Status.InterlockedOr(0, 2);
        return;
    }
    uint bin = (body * Sections + Section) * 24;

    // Mode=0은 side를 구분하지 않고 body×section의 삼각형 수를 합산한다.
    if (Mode == 0)
    {
        Bins.InterlockedAdd(bin, 1);
        return;
    }
    if (Status.Load(0) != 0) return;
    uint slot;
    Bins.InterlockedAdd(bin + 20, 1, slot);
    if (slot >= Bins.Load(bin))
    {
        Status.InterlockedOr(0, 1);
        return;
    }

    // cutter와 동일하게 삼각형마다 독립 정점을 복사해 seam과 32B 출처를 그대로 보존한다.
    // IB는 section 로컬로 다시 쓰므로 새 section draw의 baseVertex는 0이다.
    for (uint corner = 0; corner < 3; ++corner)
    {
        uint source = InputData.Load(IbOffset + (localTriangle * 3 + corner) * 4);
        uint destination = slot * 3 + corner;
        // 부호 순서가 보존되는 정수로 변환하여 정점 readback 없이 bounds를 줄인다.
        if (Padding.x != 0)
        {
            uint bounds = BodyCount * Sections * 24 + bin;
            uint3 position = InputData.Load3(VbOffset + source * Stride);
            for (uint axis = 0; axis < 3; ++axis)
            {
                uint bits = position[axis];
                uint ordered = (bits & 0x80000000) != 0 ? ~bits : bits ^ 0x80000000;
                Bins.InterlockedMin(bounds + axis * 4, ordered);
                Bins.InterlockedMax(bounds + 12 + axis * 4, ordered);
            }
        }
        for (uint b = 0; b < Stride; b += 4)
            OutputData.Store(Bins.Load(bin + 8) + destination * Stride + b,
                InputData.Load(VbOffset + source * Stride + b));
        for (uint d = 0; d < DepthStride; d += 4)
            OutputData.Store(Bins.Load(bin + 12) + destination * DepthStride + d,
                InputData.Load(DepthOffset + source * DepthStride + d));
        uint provenance = (FirstTriangle * 3 + source) * 32;
        uint output = (Bins.Load(bin + 4) * 3 + destination) * 32;
        OutputProvenance.Store4(output, Provenance.Load4(provenance));
        OutputProvenance.Store4(output + 16, Provenance.Load4(provenance + 16));
        OutputData.Store(Bins.Load(bin + 16) + destination * 4, destination);
    }
}
