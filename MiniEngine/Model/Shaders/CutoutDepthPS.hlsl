//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
// Developed by Minigraph
//
// Author(s):	James Stanard
//

#include "Common.hlsli"

struct VSOutput
{
    float4 pos : SV_Position;
    float2 uv : TexCoord0;
    float cutMask : TexCoord1;      // R of the vertex color override (>= 0.5 means hidden)
};

Texture2D<float4> baseColorTexture          : register(t0);
SamplerState baseColorSampler               : register(s0);

cbuffer MaterialConstants : register(b0)
{
    float4 baseColorFactor;
    float3 emissiveFactor;
    float normalTextureScale;
    float2 metallicRoughnessFactor;
    uint flags;
}

[RootSignature(Renderer_RootSig)]
void main(VSOutput vsOutput)
{
    if (vsOutput.cutMask >= 0.5)
        discard;
    float cutoff = f16tof32(flags >> 16);
    if (baseColorTexture.Sample(baseColorSampler, vsOutput.uv).a < cutoff)
        discard;
}
