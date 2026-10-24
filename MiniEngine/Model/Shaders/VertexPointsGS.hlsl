#include "Common.hlsli"

cbuffer PointGlobals : register(b1)
{
    float2 InvViewportSize : packoffset(c11.z);
};

// DefaultVS 출력 전체를 같은 순서로 선언해 사용하지 않는 필드도 링크 레지스터를 유지한다.
struct PointInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
#ifndef NO_TANGENT_FRAME
    float4 tangent : TANGENT;
#endif
    float2 uv0 : TEXCOORD0;
#ifndef NO_SECOND_UV
    float2 uv1 : TEXCOORD1;
#endif
    float3 worldPos : TEXCOORD2;
    float3 sunShadowCoord : TEXCOORD3;
};

struct PointOutput
{
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
};

// 원래 VS의 clip 좌표를 실제 viewport 기준 3px 사각형으로 확장한다.
[RootSignature(Renderer_RootSig)]
[maxvertexcount(4)]
void main(point PointInput input[1], inout TriangleStream<PointOutput> stream)
{
    float4 center = input[0].position;
    if (center.w <= 0 || center.z < 0 || center.z > center.w)
        return;
    const float2 corners[4] = { float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, -1) };
    for (uint i = 0; i < 4; ++i)
    {
        PointOutput output;
        output.position = center;
        output.position.xy += corners[i] * 3.0 * InvViewportSize * center.w;
        output.uv0 = input[0].uv0;
#ifndef NO_SECOND_UV
        output.uv1 = input[0].uv1;
#else
        output.uv1 = input[0].uv0;
#endif
        stream.Append(output);
    }
    stream.RestartStrip();
}
