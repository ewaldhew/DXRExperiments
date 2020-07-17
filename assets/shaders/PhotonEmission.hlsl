#include "RaytracingCommon.hlsli"

struct Payload
{
    float3 power;
    float3 position;
    float3 direction;
};

// Global root signature
StructuredBuffer<PhotonEmitter> emitters : register(t3);
RWStructuredBuffer<Photon> gOutput : register(u0);

[shader("raygeneration")]
void RayGen()
{
    uint sampleIndex = DispatchRaysIndex().x;
    uint lightIndex = DispatchRaysIndex().y;

    if (sampleIndex < emitters[lightIndex].samplesToTake) {
        // pick random point on bounding sphere
        uint2 pixIdx = DispatchRaysIndex().xy;
        uint2 numPix = DispatchRaysDimensions().xy;
        uint randSeed = initRand(pixIdx.x + pixIdx.y * numPix.x, perFrameConstants.cameraParams.frameCount);
        float3 sphereDirection = getUniformSphereSample(randSeed);

        RayDesc ray;
        ray.Origin = emitters[lightIndex].center + emitters[lightIndex].radius * sphereDirection;
        ray.Direction = -sphereDirection;
        ray.TMin = 0;
        ray.TMax = emitters[lightIndex].radius * 2.0;

        Payload payload;

        TraceRay(SceneBVH, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, MaterialSceneFlags::Emissive, 0, 0, 0, ray, payload);

        // Photon is packed and stored with correct offset.
        if (any(payload.power)) {
            uint photonIndex = emitters[lightIndex].sampleStartIndex + sampleIndex;
            gOutput[photonIndex].randSeed = initRand(photonIndex, perFrameConstants.cameraParams.frameCount);
            gOutput[photonIndex].power = payload.power;
            gOutput[photonIndex].position = payload.position;
            gOutput[photonIndex].direction = unitvec_to_spherical(payload.direction);
            gOutput[photonIndex].distTravelled = lightIndex * 100000 + sampleIndex;
        }
    }
}

void storePhoton(float3 position, float3 normal, inout Payload payload)
{
    float3 power = materialParams.emissive.rgb * materialParams.emissive.a;
    float3 direction = normal;

    payload.power = power;
    payload.position = position;
    payload.direction = direction;
}

[shader("closesthit")]
void ClosestHit(inout Payload payload, in Attributes attrib)
{
    // Load surface attributes for the hit.
    float3 vertPosition, vertNormal;
    interpolateVertexAttributes(attrib.bary, vertPosition, vertNormal);

    storePhoton(HitWorldPosition(), normalize(vertNormal), payload);
}

[shader("closesthit")]
void ClosestHit_AABB(inout Payload payload, in ProceduralPrimitiveAttributes attrib)
{
    storePhoton(HitWorldPosition(), normalize(attrib.normal), payload);
}

[shader("miss")]
void Miss(inout Payload payload)
{
    payload.power = 0.0;
}
