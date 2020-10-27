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

#if !defined(RAYTRACINGHLSLCOMPAT_H) && !defined(RASTER_PIPELINE)
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

struct Vertex
{
    XMFLOAT3 position;
    XMFLOAT3 normal;
};

struct DebugOptions
{
    UINT maxIterations;
    UINT cosineHemisphereSampling;
    UINT showIndirectDiffuseOnly;
    UINT showIndirectSpecularOnly;
    UINT showAmbientOcclusionOnly;
    UINT showGBufferAlbedoOnly;
    UINT showDirectLightingOnly;
    UINT showFresnelTerm;
    UINT noIndirectDiffuse;
    float environmentStrength;
    UINT debug;
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

//#define MAX_PHOTON_SEED_SAMPLES 500000
#define MAX_PHOTON_SEED_SAMPLES 1000000
//#define MAX_PHOTON_SEED_SAMPLES 2000000
#define PHOTON_EMISSION_GROUP_SIZE 64
#define MAX_PHOTONS (MAX_PHOTON_SEED_SAMPLES * 3)

struct PhotonEmitter
{
    XMFLOAT3 center;
    XMFLOAT3 direction;
    float radius;
    UINT samplesToTake;
    UINT sampleStartIndex;
    XMFLOAT3 pad;
};

struct PhotonEmitterPayload
{
    XMFLOAT3 power;
    XMFLOAT3 position;
    XMFLOAT3 direction;
};

#define MAX_PHOTON_DEPTH 8

struct PhotonPayload
{
    // RNG state
    UINT random;
    // Packed photon power
    //uint power;
    XMFLOAT3 power; // last component is scaling factor
    // Ray length
    float distTravelled;
    // Bounce count
    UINT bounce;

    // Outgoing payload
    XMFLOAT3 position;
    XMFLOAT3 direction;
};

#define PARTICLE_BUFFER_SIZE 32
struct PhotonSplatPayload
{
    XMFLOAT3 result;
    UINT tail;
};

#endif // RAYTRACINGHLSLCOMPAT_H
