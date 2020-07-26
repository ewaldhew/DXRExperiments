#include "RaytracingCommon.hlsli"

#define MAX_VOLUME_INTERACTIONS 6

struct VolumePayload
{
    uint originIndex;
    bool inVolume;
};

// Test if a position is in the volume
bool shootVolumeRay(inout float3 pos, float3 dir, float minT, float maxT, uint currentDepth)
{
    if (currentDepth >= MAX_RADIANCE_RAY_DEPTH) {
        return false;
    }

    RayDesc ray = { pos, minT, dir, maxT };

    VolumePayload payload;
    payload.originIndex = InstanceIndex();

    TraceRay(SceneBVH, 0, MaterialSceneFlags::Volume, 1, 0, 1, ray, payload);

    return payload.inVolume;
}

float getExtinction(float3 position)
{
    return sampleMaterialEx(materialParams.albedo, position).x;
}

bool evaluateVolumeInteraction(inout uint randSeed, inout float3 position, float3 direction, out float t, inout float3 params, uint currentDepth)
{
    float extinctionMax = materialParams.reflectivity;
    t = 0.0f;
    float3 pos;
    bool inVolume, passed;

    do { // woodcock tracking
        t -= log(nextRand(randSeed)) / extinctionMax;
        pos = position + direction * t;
        inVolume = shootVolumeRay(pos, direction, 0, RAY_MAX_T, currentDepth);
    } while (inVolume && getExtinction(pos) < nextRand(randSeed) * extinctionMax);

    position = pos;
    // x - extinction (ka), y - scattering (ks)
    params = sampleMaterialEx(materialParams.albedo, pos).xyz;

    return inVolume;
}

bool exitingVolume(float3 normal, float3 rayDir, uint fromIndex)
{
    return dot(normal, rayDir) >= 0 || fromIndex == InstanceIndex();
}

[shader("closesthit")]
void VolumeClosestHit(inout VolumePayload payload, Attributes attrib)
{
    float3 vertPosition, vertNormal;
    interpolateVertexAttributes(attrib.bary, vertPosition, vertNormal);

    payload.inVolume = exitingVolume(normalize(vertNormal), WorldRayDirection(), payload.originIndex);
}

[shader("closesthit")]
void VolumeClosestHit_AABB(inout VolumePayload payload, in ProceduralPrimitiveAttributes attr)
{
    payload.inVolume = exitingVolume(normalize(attr.normal), WorldRayDirection(), payload.originIndex);
}

[shader("miss")]
void VolumeMiss(inout VolumePayload payload)
{
    payload.inVolume = false;
}
