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
    nointerpolation float3 overlayColor : COLOR0;
};

struct PointOutput
{
    float4 position : SV_POSITION;
    float2 uv0 : TEXCOORD0;
    float2 uv1 : TEXCOORD1;
    nointerpolation float3 overlayColor : COLOR0;
};

// VS 색상을 보존하며 clip 좌표를 실제 viewport 기준 3px 사각형으로 확장한다.
[RootSignature(Renderer_RootSig)]
[maxvertexcount(4)]
void main(point PointInput input[1], inout TriangleStream<PointOutput> stream)
{
    // 카메라 뒤쪽과 깊이 범위 밖의 정점은 제외한다.
    float4 center = input[0].position;
    if (center.w <= 0 || center.z < 0 || center.z > center.w)
        return;

    // 같은 UV와 메시 색상을 사각형의 네 모서리에 전달한다.
    const float2 corners[4] = { float2(-1, 1), float2(1, 1), float2(-1, -1), float2(1, -1) };
    for (uint i = 0; i < 4; ++i)
    {
        PointOutput output;
        output.position = center;
        output.position.xy += corners[i] * 3.0 * InvViewportSize * center.w;
        output.uv0 = input[0].uv0;
        output.overlayColor = input[0].overlayColor;
#ifndef NO_SECOND_UV
        output.uv1 = input[0].uv1;
#else
        output.uv1 = input[0].uv0;
#endif
        stream.Append(output);
    }

    // 다음 정점의 사각형과 스트립을 분리한다.
    stream.RestartStrip();
}
