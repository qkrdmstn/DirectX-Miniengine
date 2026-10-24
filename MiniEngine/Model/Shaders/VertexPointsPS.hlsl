#include "Common.hlsli"

Texture2D<float4> baseColorTexture : register(t0);
SamplerState baseColorSampler : register(s0);

cbuffer MaterialConstants : register(b0)
{
    float4 baseColorFactor;
    float3 emissiveFactor;
    float normalTextureScale;
    float2 metallicRoughnessFactor;
    uint flags;
};

// 정점 위치의 재질 알파를 검사하고 흰색 점을 출력한다.
[RootSignature(Renderer_RootSig)]
float4 main(float4 position : SV_POSITION, float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1) : SV_Target0
{
    float2 uv = (flags & 1) ? uv1 : uv0;
    float alpha = baseColorTexture.SampleLevel(baseColorSampler, uv, 0).a * baseColorFactor.a;
    if ((flags & (1u << 6)) && alpha < f16tof32(flags >> 16))
        discard;
    alpha = (flags & (1u << 7)) ? alpha : 1.0;
    return float4(float3(1.0, 1.0, 1.0) * alpha, alpha);
}
