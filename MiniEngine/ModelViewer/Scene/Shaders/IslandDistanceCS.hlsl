cbuffer Parameters : register(b0)
{
    float4 Plane;
    float4x4 World;
    uint Section, Draws, Capacity, Padding;
};
struct DrawInput
{
    uint VbOffset, Stride, IbOffset, Index32;
    uint StartIndex, BaseVertex, Triangles, PoseOffset;
    uint FirstTriangle, MaskOffset, Flags, SectionId;
    uint DrawId, JointCount, VertexCount, WordOffset;
};
ByteAddressBuffer Sources : register(t0);
ByteAddressBuffer Meta : register(t1);
StructuredBuffer<DrawInput> Inputs : register(t2);
ByteAddressBuffer Source : register(t3);
ByteAddressBuffer Pose : register(t4);
StructuredBuffer<uint> VertexCell : register(t5);
RWStructuredBuffer<float3> CornerDistance : register(u0);
RWStructuredBuffer<uint3> CornerCell : register(u1);

// 추출과 같은 제출에서 PG를 읽어 복사 이동 전 월드 거리와 셀을 보존한다.
[numthreads(64,1,1)]
void main(uint3 id : SV_DispatchThreadID)
{
    // capacity 밖과 GPU 선택 개수 밖 슬롯은 읽지 않는다.
    uint t=id.x+id.y*(65535*64);
    if(t>=Capacity || t>=Meta.Load(0)) return;
    uint3 origin=Sources.Load3(t*12);
    if(origin.x!=Section) return;

    // section과 draw 출처에서 원본 인덱스 형식과 PG 오프셋을 찾는다.
    uint draw=0;
    while(draw<Draws && (Inputs[draw].SectionId!=origin.x || Inputs[draw].DrawId!=origin.y)) draw++;
    if(draw==Draws) return;
    DrawInput s=Inputs[draw];
    float3 distances; uint3 cells;
    [unroll] for(uint k=0;k<3;k++)
    {
        // R16/R32 인덱스에 baseVertex를 더하고 이미 frame이 적용된 PG를 월드로 옮긴다.
        uint at=s.StartIndex+origin.z*3+k;
        uint word=Source.Load(s.IbOffset+(s.Index32?at*4:(at/2)*4));
        uint v=(s.Index32?word:((word>>((at&1)*16))&65535))+s.BaseVertex;
        float3 p=asfloat(Pose.Load3(s.PoseOffset+v*40));
        distances[k]=dot(Plane,float4(mul(World,float4(p,1)).xyz,1));
        cells[k]=VertexCell[v];
    }

    // 패치 순서를 유지해 이후 단계가 PG 수명과 무관하게 읽게 한다.
    CornerDistance[t]=distances;
    CornerCell[t]=cells;
}
