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
// Author:   James Stanard
//

#pragma once

#include "../Core/Math/Matrix3.h"
#include "../Core/Math/Matrix4.h"
#include <cstdint>

__declspec(align(256)) struct MeshConstants
{
    Math::Matrix4 World;         // Object to world
    Math::Matrix3 WorldIT;       // Object normal to world normal
};

// The order of textures for PBR materials
enum { kBaseColor, kMetallicRoughness, kOcclusion, kEmissive, kNormal, kNumTextures };

__declspec(align(256)) struct MaterialConstants
{
    float baseColorFactor[4]; // default=[1,1,1,1]
    float emissiveFactor[3]; // default=[0,0,0]
    float normalTextureScale; // default=1
    float metallicFactor; // default=1
    float roughnessFactor; // default=1
    union
    {
        uint32_t flags;
        struct
        {
            // UV0 or UV1 for each texture
            uint32_t baseColorUV : 1;
            uint32_t metallicRoughnessUV : 1;
            uint32_t occlusionUV : 1;
            uint32_t emissiveUV : 1;
            uint32_t normalUV : 1;

            // Three special modes
            uint32_t twoSided : 1;
            uint32_t alphaTest : 1;
            uint32_t alphaBlend : 1;

            uint32_t _pad : 8;

            uint32_t alphaRef : 16; // half float
        };
    };
};

// 웨이트 뷰가 쓰는 joint 색 표의 최대 크기 (팔레트당 joint 수)
static const uint32_t kMaxJointColors = 256;

__declspec(align(256)) struct GlobalConstants
{
    Math::Matrix4 ViewProjMatrix;
    Math::Matrix4 SunShadowMatrix;
    Math::Vector3 CameraPos;
    Math::Vector3 SunDirection;
    Math::Vector3 SunIntensity;
    float IBLRange;
    float IBLBias;
    uint32_t Reserved;                  // 예전 DebugJointIndex 자리 (셰이더와 배치를 맞추려고 남겼다)
    uint32_t WeightDebugEnabled;
    float JointColors[kMaxJointColors][4];      // 웨이트 뷰: joint 번호별 색 (정점 색 = Σ 가중치 × joint 색)
};
