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
// Author(s):  James Stanard
//

#include "Common.hlsli"
#include "PosedVertices.hlsli"

#ifdef ENABLE_SKINNING
//#undef ENABLE_SKINNING
#endif

cbuffer MeshConstants : register(b0)
{
    float4x4 WorldMatrix : packoffset(c0);   // Object to world
    float3x3 WorldIT : packoffset(c4);       // Object normal to world normal
    float4x4 ObjectWorldMatrix : packoffset(c7);
    float3x3 ObjectWorldIT : packoffset(c11);
    uint UsePosedVertices : packoffset(c14.x);
    float3 OverlayColor : packoffset(c14.y);
};

cbuffer GlobalConstants : register(b1)
{
    float4x4 ViewProjMatrix;
    float4x4 SunShadowMatrix;
    float3 ViewerPos;
    float3 SunDirection;
    float3 SunIntensity;
}

#ifdef ENABLE_SKINNING
struct Joint
{
    float4x4 PosMatrix;
    float4x3 NrmMatrix; // Inverse-transpose of PosMatrix
};

StructuredBuffer<Joint> Joints : register(t20);
#endif

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
#ifndef NO_TANGENT_FRAME
    float4 tangent : TANGENT;
#endif
    float2 uv0 : TEXCOORD0;
#ifndef NO_SECOND_UV
    float2 uv1 : TEXCOORD1;
#endif
#ifdef ENABLE_SKINNING
    uint4 jointIndices : BLENDINDICES;
    float4 jointWeights : BLENDWEIGHT;
#endif
};

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
    nointerpolation float3 overlayColor : COLOR0;
};

[RootSignature(Renderer_RootSig)]
// 공통 pose에 객체 변환을 적용하고 메시의 선·점 색상을 다음 단계에 전달한다.
VSOutput main(VSInput vsInput, uint vertexId : SV_VertexID)
{
    // 원본 정점의 위치와 표면 방향을 준비한다.
    VSOutput vsOutput;
    float4 position = float4(vsInput.position, 1.0);
    float3 normal = vsInput.normal * 2 - 1;
#ifndef NO_TANGENT_FRAME
    float4 tangent = vsInput.tangent * 2 - 1;
#endif

#ifdef ENABLE_SKINNING
    // Compute pose가 없으면 기존 본 변환을 적용한다.
    if (UsePosedVertices == 0)
    {
        // 입력 가중치 합을 1로 정규화한다(원본 주석: weights should be normalized already).
        float4 weights = vsInput.jointWeights / dot(vsInput.jointWeights, 1);

        // 가중 본 행렬로 위치를 변환한다.
        float4x4 skinPosMat =
            Joints[vsInput.jointIndices.x].PosMatrix * weights.x +
            Joints[vsInput.jointIndices.y].PosMatrix * weights.y +
            Joints[vsInput.jointIndices.z].PosMatrix * weights.z +
            Joints[vsInput.jointIndices.w].PosMatrix * weights.w;

        position = mul(skinPosMat, position);

        // 가중 법선 행렬로 법선과 접선을 변환한다.
        float4x3 skinNrmMat =
            Joints[vsInput.jointIndices.x].NrmMatrix * weights.x +
            Joints[vsInput.jointIndices.y].NrmMatrix * weights.y +
            Joints[vsInput.jointIndices.z].NrmMatrix * weights.z +
            Joints[vsInput.jointIndices.w].NrmMatrix * weights.w;

        normal = mul(skinNrmMat, normal).xyz;
#ifndef NO_TANGENT_FRAME
        tangent.xyz = mul(skinNrmMat, tangent.xyz).xyz;
#endif
    }

#endif

    // Compute pose가 있으면 준비된 위치와 표면 방향을 읽는다.
    if (UsePosedVertices != 0)
    {
        position = float4(LoadPosedPosition(vertexId), 1);
        normal = asfloat(PosedVertices.Load3(vertexId * 40 + 12));
#ifndef NO_TANGENT_FRAME
        tangent = asfloat(PosedVertices.Load4(vertexId * 40 + 24));
#endif
    }

    // 월드·투영 좌표와 재질 UV를 다음 단계에 전달한다.
    float4x4 world = UsePosedVertices != 0 ? ObjectWorldMatrix : WorldMatrix;
    float3x3 worldIT = UsePosedVertices != 0 ? ObjectWorldIT : WorldIT;
    vsOutput.worldPos = mul(world, position).xyz;
    vsOutput.position = mul(ViewProjMatrix, float4(vsOutput.worldPos, 1.0));
    vsOutput.sunShadowCoord = mul(SunShadowMatrix, float4(vsOutput.worldPos, 1.0)).xyz;
    vsOutput.normal = mul(worldIT, normal);
#ifndef NO_TANGENT_FRAME
    vsOutput.tangent = float4(mul(worldIT, tangent.xyz), tangent.w);
#endif
    vsOutput.uv0 = vsInput.uv0;
#ifndef NO_SECOND_UV
    vsOutput.uv1 = vsInput.uv1;
#endif

    // 보간 없는 메시 색상은 선과 점의 PS에서만 사용한다.
    vsOutput.overlayColor = OverlayColor;
    return vsOutput;
}
