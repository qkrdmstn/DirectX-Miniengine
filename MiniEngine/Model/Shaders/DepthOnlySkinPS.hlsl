//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//

#include "Common.hlsli"

struct VSOutput
{
    float4 pos : SV_Position;
    float cutMask : TexCoord1;      // R of the vertex color override (>= 0.5 means hidden)
};

[RootSignature(Renderer_RootSig)]
void main(VSOutput vsOutput)
{
    if (vsOutput.cutMask >= 0.5)
        discard;
}
