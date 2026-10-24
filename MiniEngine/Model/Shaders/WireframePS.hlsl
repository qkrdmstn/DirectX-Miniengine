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

// DefaultVS의 전체 출력 순서와 조건부 필드를 유지해 입력 레지스터·마스크를 맞춘다.
struct VSOutput
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

// 재질의 알파 검사·블렌딩을 유지하고 조명과 무관한 흰색 와이어를 출력한다.
[RootSignature(Renderer_RootSig)]
float4 main(VSOutput vsOutput) : SV_Target0
{
#ifdef NO_SECOND_UV
    float2 uv = vsOutput.uv0;
#else
    float2 uv = (flags & 1) ? vsOutput.uv1 : vsOutput.uv0;
#endif
    float alpha = baseColorTexture.Sample(baseColorSampler, uv).a * baseColorFactor.a;
    if ((flags & (1u << 6)) && alpha < f16tof32(flags >> 16))
        discard;
    alpha = (flags & (1u << 7)) ? alpha : 1.0;
    return float4(float3(1.0, 1.0, 1.0) * alpha, alpha);
}
