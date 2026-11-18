// 컴포넌트 상태로 원본 IB를 퇴화 치환한다.
// Source와 ReplacedIB는 별도 자원이며 OutputFirstWord로 출력 IB 구간의 시작을 뺀다.
// 호출자는 IB 범위를 DWORD 단위로 감싸고 ceil(WordCount / 64)개 그룹을 제출한다.
cbuffer Parameters : register(b0)
{
    uint FirstWord, WordCount, Draws, OutputFirstWord;
};

// PatchExtractCS의 입력 배치와 section/draw 누적 FirstTriangle을 그대로 사용한다.
struct DrawInput
{
    uint VbOffset, Stride, IbOffset, Index32;
    uint StartIndex, BaseVertex, Triangles, PoseOffset;
    uint FirstTriangle, MaskOffset, Flags, SectionId;
    uint DrawId, JointCount, VertexCount, Reserved1;
};
ByteAddressBuffer Source : register(t0);
StructuredBuffer<DrawInput> Inputs : register(t1);
StructuredBuffer<uint> State : register(t2);
RWByteAddressBuffer ReplacedIB : register(u0);

// 제거 비트와 진단 선택(0 남은 면, 1 처음 복사된 면, 2 재복사된 면)에 따라 원본 또는 퇴화 인덱스를 반환한다.
uint Replacement(DrawInput draw, uint index, uint original)
{
    // 기본 경로는 제거되지 않은 면을 보존하고 진단 경로는 선택한 제거 면만 보존한다.
    uint triangleId = index / 3;
    uint state = State[draw.FirstTriangle + triangleId];
    bool hidden = (state & 0x80000000u) != 0;
    bool recopied = (state & 0x40000000u) != 0;
    bool keep = draw.Reserved1 == 0 ? !hidden : (draw.Reserved1 == 1 ? hidden && !recopied : hidden && recopied);
    if (keep)
        return original;

    // baseVertex는 DrawIndexed에서 더하므로 IB에는 원래 상대 인덱스를 쓴다.
    uint first = draw.StartIndex + triangleId * 3;
    uint word = Source.Load(draw.IbOffset + (draw.Index32 != 0 ? first * 4 : (first / 2) * 4));
    return draw.Index32 != 0 ? word : (word >> ((first & 1) * 16)) & 65535u;
}

// DWORD 하나를 한 스레드가 재구성하여 인접 R16 삼각형 사이의 쓰기 경쟁을 막는다.
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // 큰 IB도 PatchExtractCS와 같은 2차원 dispatch 번호로 처리한다.
    uint thread = id.x + id.y * (65535 * 64);
    if (thread >= WordCount)
        return;
    uint address = (FirstWord + thread) * 4;
    uint original = Source.Load(address);
    uint result = original;

    // draw 사이의 빈 공간과 IB 경계의 사용하지 않는 반쪽은 원본 그대로 복원한다.
    for (uint i = 0; i < Draws; ++i)
    {
        DrawInput draw = Inputs[i];
        uint width = draw.Index32 != 0 ? 4 : 2;
        uint begin = draw.IbOffset + draw.StartIndex * width;
        uint end = begin + draw.Triangles * 3 * width;
        if (address >= end || address + 4 <= begin)
            continue;

        // R32는 DWORD 전체를 쓰고 R16은 두 반쪽을 독립적으로 해석한다.
        if (draw.Index32 != 0)
            result = Replacement(draw, (address - begin) / 4, original);
        else
        {
            [unroll] for (uint halfIndex = 0; halfIndex < 2; ++halfIndex)
            {
                uint at = address + halfIndex * 2;
                if (at < begin || at >= end)
                    continue;
                uint shift = halfIndex * 16;
                uint value = Replacement(draw, (at - begin) / 2, (original >> shift) & 65535u);
                result = (result & ~(65535u << shift)) | (value << shift);
            }
        }
    }

    // 이전 치환 IB를 읽지 않아 마스크 해제와 소유권 변경에도 원본에서 재생성한다.
    ReplacedIB.Store(address - OutputFirstWord * 4, result);
}
