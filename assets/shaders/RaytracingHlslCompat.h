//*********************************************************
//
// Copyright (c) Microsoft. All rights reserved.
// This code is licensed under the MIT License (MIT).
// THIS CODE IS PROVIDED *AS IS* WITHOUT WARRANTY OF
// ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING ANY
// IMPLIED WARRANTIES OF FITNESS FOR A PARTICULAR
// PURPOSE, MERCHANTABILITY, OR NON-INFRINGEMENT.
//
//*********************************************************

#ifndef RAYTRACINGHLSLCOMPAT_H
#define RAYTRACINGHLSLCOMPAT_H

#ifdef HLSL
#include "HlslCompat.h"
#else
using namespace DirectX;

// Shader will use byte encoding to access indices.
typedef UINT16 Index;
#endif

#include "CommonHlslCompat.h"

struct ShadowPayload
{
    float lightVisibility;
};

struct Attributes
{
    XMFLOAT2 bary;
};

struct ProceduralPrimitiveAttributes
{
    XMFLOAT3 normal;
};

struct Vertex
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
};

struct CameraParams
{
    XMFLOAT4 worldEyePos;
    XMFLOAT4 U;
    XMFLOAT4 V;
    XMFLOAT4 W;
    XMFLOAT2 frustumNH;
    XMFLOAT2 frustumNV;
    XMFLOAT2 frustumNearFar;
    XMFLOAT2 jitters;
    UINT frameCount;
    UINT accumCount;
    XMFLOAT2 padding;
};

struct PerFrameConstants
{
    CameraParams cameraParams;
    DebugOptions options;
};

namespace MaterialSceneFlags
{
    enum Enum
    {
        None     = 1 << 0,
        Emissive = 1 << 1,
        Volume   = 1 << 2,
    };
}

#define MAX_PHOTON_SEED_SAMPLES 100000
#define PHOTON_EMISSION_GROUP_SIZE 64
#define MAX_PHOTONS (MAX_PHOTON_SEED_SAMPLES * 10)

struct PhotonEmitter
{
    XMFLOAT3 center;
    XMFLOAT3 direction;
    float radius;
    UINT samplesToTake;
    UINT sampleStartIndex;
    XMFLOAT3 pad;
};

#endif // RAYTRACINGHLSLCOMPAT_H
