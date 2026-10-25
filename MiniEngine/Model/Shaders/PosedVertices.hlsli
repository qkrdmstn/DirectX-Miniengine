ByteAddressBuffer PosedVertices : register(t21);

// draw의 baseVertex는 SRV 시작점에 반영하고 원본 인덱스로 posed local 위치를 읽는다.
float3 LoadPosedPosition(uint vertexId)
{
    return asfloat(PosedVertices.Load3(vertexId * 40));
}
