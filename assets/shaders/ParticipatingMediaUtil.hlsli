#ifdef RASTER_PIPELINE
#include "RasterCommon.hlsli"
#else
#include "RaytracingCommon.hlsli"
#endif

struct VolumeParams
{
    float3 emission;
    float absorption;
    float scattering;
    int phase_func_type;
};

float4 sampleMaterial1(float4 materialParam, float3 objPos)
{
    if (materialParam.w < 0.0) {
        uint materialTexIndex = materialParam.x;
        float3 texPos = mul(float4(objPos, 1), (float4x4) texParams[materialTexIndex].objectSpaceToTex).xyz;
        return materialParamsTex[materialTexIndex].SampleLevel(matTexSampler, texPos, 0.0);
    }
    else {
        return materialParam;
    }
}

void collectMaterialParams1(inout MaterialParams mat, float3 positionObjSpace)
{
    if (mat.type > MaterialType::__UniformMaterials) { // use texture
        mat.albedo = sampleMaterial1(mat.albedo, positionObjSpace);
        mat.specular = sampleMaterial1(mat.specular, positionObjSpace);
        mat.emissive = sampleMaterial1(mat.emissive, positionObjSpace);
    }
}

VolumeParams getVolumeParams(MaterialParams mat)
{
    VolumeParams vol;
    vol.emission = mat.emissive.rgb * mat.emissive.a;
    vol.absorption = mat.albedo.x;
    vol.scattering = mat.albedo.y;
    vol.phase_func_type = int(mat.IoR);
    return vol;
}

float3 samplePhaseFunc(int phaseFuncType)
{
    return 0.f;
}

float evalPhaseFuncPdf(uint phaseFuncType, float3 inDir, float3 outDir)
{
    { // isotropic
        return 1.0f / (4 * M_PI);
    }
}
