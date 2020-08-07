#ifndef RASTER_COMMON_HLSLI
#define RASTER_COMMON_HLSLI

#define HLSL
#include "RasterHlslCompat.h"
#include "RaytracingUtils.hlsli"

////////////////////////////////////////////////////////////////////////////////
// Global root signature
////////////////////////////////////////////////////////////////////////////////

ConstantBuffer<PerFrameConstantsRaster> perFrameConstants : register(b0);

StructuredBuffer<DirectionalLightParams> directionalLights : register(t1);
StructuredBuffer<PointLightParams> pointLights : register(t2);

SamplerState cubeSampler : register(s0);
SamplerState matTexSampler : register(s1);

Texture3D<float4> materialParamsTex[10000] : register(t1, space9);
StructuredBuffer<MaterialTextureParams> texParams : register(t0, space9);

cbuffer MaterialConstants : register(b0, space1)
{
    MaterialParams materialParams;
}

Texture2D envMap : register(t0, space2);
TextureCube envCubemap : register(t1, space2);

////////////////////////////////////////////////////////////////////////////////
// Common routines
////////////////////////////////////////////////////////////////////////////////

float3 sampleEnvironment(float3 direction)
{
    // cubemap
    float4 envSample = envCubemap.SampleLevel(cubeSampler, direction, 0.0);

    // lat-long environment map
    // float2 uv = wsVectorToLatLong(WorldRayDirection().xyz);
    // float4 envSample = envMap.SampleLevel(cubeSampler, uv, 0.0);

    return envSample.rgb;
}

float4 sampleMaterial(float4 materialParam, float3 positionObjSpace)
{
    if (materialParam.w < 0.0) {
        uint materialTexIndex = materialParam.x;
        float3 texPos = mul(float4(positionObjSpace, 1), (float4x4) texParams[materialTexIndex].objectSpaceToTex).xyz;
        return materialParamsTex[materialTexIndex].SampleLevel(matTexSampler, texPos, 0.0);
    }
    else {
        return materialParam;
    }

}

void collectMaterialParams(inout MaterialParams mat, float3 positionObjSpace)
{
    if (materialParams.type > MaterialType::__UniformMaterials) { // use texture
        mat.albedo = sampleMaterial(materialParams.albedo, positionObjSpace);
        mat.specular = sampleMaterial(materialParams.specular, positionObjSpace);
        mat.emissive = sampleMaterial(materialParams.emissive, positionObjSpace);
    }
}

#endif // RASTER_COMMON_HLSLI
