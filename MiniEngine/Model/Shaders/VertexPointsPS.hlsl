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

// 정점 위치의 재질 알파를 유지하며 메시별 색상의 점을 출력한다.
[RootSignature(Renderer_RootSig)]
float4 main(float4 position : SV_POSITION, float2 uv0 : TEXCOORD0, float2 uv1 : TEXCOORD1,
    nointerpolation float3 overlayColor : COLOR0) : SV_Target0
{
    // 원래 재질의 알파 샘플과 컷아웃 검사를 유지한다.
    float2 uv = (flags & 1) ? uv1 : uv0;
    float alpha = baseColorTexture.SampleLevel(baseColorSampler, uv, 0).a * baseColorFactor.a;
    if ((flags & (1u << 6)) && alpha < f16tof32(flags >> 16))
        discard;

    // 기존 premultiplied alpha 정책으로 메시 색상을 출력한다.
    alpha = (flags & (1u << 7)) ? alpha : 1.0;
    return float4(overlayColor * alpha, alpha);
}
