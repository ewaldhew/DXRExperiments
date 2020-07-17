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

struct DirectionalLightParams
{
    XMFLOAT4 forwardDir;
    XMFLOAT4 color; // radiant intensity at unit distance
};

struct PointLightParams
{
    XMFLOAT4 worldPos;
    XMFLOAT4 color;
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

// Attributes per primitive instance.
struct PrimitiveInstanceConstants
{
    //UINT instanceIndex;  // Used to index into per frame attributes
    UINT primitiveType; // Procedural primitive type
    UINT pad1;
    UINT pad2;
    UINT pad3;
    XMMATRIX localSpaceToBottomLevelAS;   // Matrix from local primitive space to bottom-level object space.
    XMMATRIX bottomLevelASToLocalSpace;   // Matrix from bottom-level object space to local primitive space.
};

struct MaterialParams
{
    XMFLOAT4 albedo;
    XMFLOAT4 specular;
    XMFLOAT4 emissive;
    float reflectivity;
    float roughness;
    float IoR;
    UINT type; // 0: diffuse, 1: glossy, 2: specular (glass)
};

namespace MaterialSceneFlags
{
    enum Enum
    {
        None     = 1 << 0,
        Emissive = 1 << 1,
    };
}

struct PhotonMappingConstants
{
    XMUINT2 numTiles;
};

// Number of metaballs to use within an AABB.
#define N_METABALLS 3    // = {3, 5}

// Limitting calculations only to metaballs a ray intersects can speed up raytracing
// dramatically particularly when there is a higher number of metaballs used.
// Use of dynamic loops can have detrimental effects to performance for low iteration counts
// and outweighing any potential gains from avoiding redundant calculations.
// Requires: USE_DYNAMIC_LOOPS set to 1 to take effect.
#if N_METABALLS >= 5
#define USE_DYNAMIC_LOOPS 1
#define LIMIT_TO_ACTIVE_METABALLS 1
#else
#define USE_DYNAMIC_LOOPS 0
#define LIMIT_TO_ACTIVE_METABALLS 0
#endif

#define N_FRACTAL_ITERATIONS 4      // = <1,...>

namespace PrimitiveType {
    enum Enum
    {
        AnalyticPrimitive_AABB = 0,
        AnalyticPrimitive_Spheres,
        VolumetricPrimitive_Metaballs,
        SignedDistancePrimitive_MiniSpheres,
        SignedDistancePrimitive_IntersectedRoundCube,
        SignedDistancePrimitive_SquareTorus,
        SignedDistancePrimitive_TwistedTorus,
        SignedDistancePrimitive_Cog,
        SignedDistancePrimitive_Cylinder,
        SignedDistancePrimitive_FractalPyramid,
    };
}

#define MAX_PHOTON_SEED_SAMPLES 100000
#define PHOTON_EMISSION_GROUP_SIZE 64
#define MAX_PHOTONS (MAX_PHOTON_SEED_SAMPLES * 10)

struct PhotonEmitter
{
    XMFLOAT3 center;
    float radius;
    UINT samplesToTake;
    UINT sampleStartIndex;
    XMUINT2 pad2;
};

struct Photon
{
    XMFLOAT3 position;
    XMFLOAT3 power;
    XMFLOAT3 direction;
    XMFLOAT3 normal;
    float distTravelled;
    UINT randSeed;
    XMUINT2 padding;
};

#endif // RAYTRACINGHLSLCOMPAT_H
