#include "RaytracingCommon.hlsli"

#define MAX_VOLUME_INTERACTIONS 10

struct VolumePayload
{
    bool exited;
};

// Test if the ray exits the volume
bool shootVolumeRay(float3 orig, float3 dir, float minT, float maxT, uint currentDepth)
{
    if (currentDepth >= MAX_RADIANCE_RAY_DEPTH) {
        return true;
    }

    RayDesc ray = { orig, minT, dir, maxT };

    VolumePayload payload = { false };

    TraceRay(SceneBVH, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, MaterialSceneFlags::Volume, 2, 0, 2, ray, payload);
    return payload.exited;
}

float getExtinction(float3 position)
{
    return sampleMaterialEx(materialParams.albedo, position).x;
}

bool evaluateVolumeInteraction(inout uint randSeed, inout float3 position, float3 direction, inout float3 params, uint currentDepth)
{
    float extinctionMax = materialParams.reflectivity;
    float t = 0.0f;
    float3 pos;

    do {
        t -= log(nextRand(randSeed)) / extinctionMax;
        pos = position + direction * t;
        bool exitedVolume = shootVolumeRay(position, direction, RAY_EPSILON, t, currentDepth);
        if (exitedVolume) {
            return false;
        }
    } while (getExtinction(pos) < nextRand(randSeed) * extinctionMax);

    position = pos;
    params = sampleMaterialEx(materialParams.albedo, pos).xyz;

    return true;
}

[shader("closesthit")]
void VolumeClosestHit(inout VolumePayload payload, Attributes attrib)
{
    float3 vertPosition, vertNormal;
    interpolateVertexAttributes(attrib.bary, vertPosition, vertNormal);

    payload.exited = dot(vertNormal, WorldRayDirection().xyz) > 0.0;
}

[shader("closesthit")]
void VolumeClosestHit_AABB(inout VolumePayload payload, in ProceduralPrimitiveAttributes attr)
{
    payload.exited = dot(normalize(attr.normal), WorldRayDirection().xyz) > 0.0;
}

[shader("miss")]
void VolumeMiss(inout VolumePayload payload)
{
    // no-op
}
