cbuffer Constants : register(b0)
{
    float4 Plane;
    float4x4 LocalToPlane;
    float2 HalfSize;
    float Epsilon;
    uint NodeCount;
};
RWByteAddressBuffer Votes : register(u0);
RWByteAddressBuffer Result : register(u1);

// 단일 그룹에서 64비트 최댓값을 고르고 동점에는 작은 NodeId를 유지한다.
[numthreads(1, 1, 1)]
void main()
{
    // 오름차순으로 순회하며 양수 투표만 선택한다.
    uint bestNode = 0xffffffffu;
    uint2 best = 0;
    for (uint node = 0; node < NodeCount; ++node)
    {
        uint2 vote = Votes.Load2(node * 8);
        if (vote.y > best.y || (vote.y == best.y && vote.x > best.x))
        {
            best = vote;
            bestNode = node;
        }
    }

    // SP-2는 선택 본과 교차 수만 게시하고 패치 수는 다음 단계에 맡긴다.
    Result.Store3(0, uint3(bestNode, Votes.Load(NodeCount * 8), 0));
}
