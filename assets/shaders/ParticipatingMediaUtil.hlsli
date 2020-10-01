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
    float3 phase_func_params;
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
    vol.phase_func_params = mat.specular.xyz;
    return vol;
}

float3 samplePhaseFunc(int phaseFuncType, float3 pfParam, float3 inDir, out float pdf, inout uint randSeed)
{
    float3 rayDir = 0.f;
    pdf = 0.f;

    switch (phaseFuncType) {
    case 0: { // isotropic
        rayDir = getUniformSphereSample(randSeed);
        pdf = 1.0f / (4 * M_PI);
        break;
    }
    case 1: { // schlick approx for HG
        float g = pfParam.x;
        float k = 1.55*g - 0.55*g*g;

        float3 bitangent = getPerpendicularVector(inDir);
        float3 tangent = cross(bitangent, inDir);

        // Blasi et. al. Eqn 12
        float cosTheta = (2*nextRand(randSeed) + k - 1) / (2*k*nextRand(randSeed) - k + 1);
        float sinTheta = sqrt(1.0 - cosTheta * cosTheta);
        float phi = 2.0f * M_PI * nextRand(randSeed);

        pdf = ((1 - k*k) / pow(1 - k*cosTheta, 2.f)) / (4 * M_PI);

        float x = sinTheta * cos(phi);
        float z = sinTheta * sin(phi);
        float y = cosTheta;
        rayDir = x * tangent + y * inDir + z * bitangent;
        break;
    }
    }

    return rayDir;
}

float evalPhaseFuncPdf(uint phaseFuncType, float3 pfParam, float3 inDir, float3 outDir)
{
    switch (phaseFuncType) {
    case 0: { // isotropic
        return 1.0f / (4 * M_PI);
    }
    case 1: {
        float cosTheta = dot(inDir, outDir);
        float g = pfParam.x;
        float k = 1.55*g - 0.55*g*g;
        return ((1 - k*k) / pow(1 - k*cosTheta, 2.f)) / (4 * M_PI);
    }
    default: {
        return 0.f;
    }
    }
}
