#include "RaytracingCommon.hlsli"

// Global root signature
StructuredBuffer<PhotonEmitter> emitters : register(t3);
RWStructuredBuffer<Photon> gOutput : register(u0);

// Local root signature
Buffer<float> triangleCdf : register(t2, space1);

[shader("raygeneration")]
void RayGen()
{
    uint sampleIndex = DispatchRaysIndex().x;
    uint lightIndex = DispatchRaysIndex().y;

    if (sampleIndex < emitters[lightIndex].samplesToTake) {
        PhotonEmitterPayload payload;

        do {
            float3 sphereDirection = emitters[lightIndex].direction;

            RayDesc ray;
            ray.Origin = emitters[lightIndex].center.xyz + emitters[lightIndex].radius * sphereDirection;
            ray.Direction = -sphereDirection;
            ray.TMin = 0;
            ray.TMax = emitters[lightIndex].radius * 2.0;

            TraceRay(SceneBVH, RAY_FLAG_FORCE_OPAQUE, MaterialSceneFlags::Emissive, 0, 0, 0, ray, payload);
        } while (!any(payload.power));

        {
            // Photon is packed and stored with correct offset.
            uint photonIndex = emitters[lightIndex].sampleStartIndex + sampleIndex;
            gOutput[photonIndex].randSeed = initRand(photonIndex, perFrameConstants.cameraParams.frameCount);
            gOutput[photonIndex].power = payload.power;
            gOutput[photonIndex].position = payload.position;
            gOutput[photonIndex].direction = unitvec_to_spherical(payload.direction);
            gOutput[photonIndex].distTravelled = 0.0;
        }
    }
}

void storePhoton(float3 position, float3 normal, inout PhotonEmitterPayload payload, inout uint randSeed)
{
    uint lightIndex = DispatchRaysIndex().y;
    float proportion = 1 / float(emitters[lightIndex].samplesToTake);
    float3 power = materialParams.emissive.rgb * materialParams.emissive.a * proportion;
    float3 direction = getUniformHemisphereSample(randSeed, normal);

    payload.power = power;
    payload.position = position;
    payload.direction = direction;
}

[shader("closesthit")]
void ClosestHit(inout PhotonEmitterPayload payload, in Attributes attrib)
{
    uint2 pixIdx = DispatchRaysIndex().xy;
    uint2 numPix = DispatchRaysDimensions().xy;
    uint randSeed = initRand(pixIdx.x + pixIdx.y * numPix.x, perFrameConstants.cameraParams.frameCount);

    // pick a random triangle from the mesh
    float rand = nextRand(randSeed);
    uint triangleIndex = 0;
    while (rand > triangleCdf[triangleIndex]) {
        triangleIndex++;
    }

    // https://chrischoy.github.io/research/barycentric-coordinate-for-mesh-sampling/
    float2 r = float2(nextRand(randSeed), nextRand(randSeed));
    float sqrtr1 = sqrt(r.x);
    float2 randBary = float2(sqrtr1 * (1 - r.y), sqrtr1 * r.y);

    float3 vertPosition, vertNormal;
    interpolateVertexAttributes1(triangleIndex, randBary, vertPosition, vertNormal);

    storePhoton(vertPosition, normalize(vertNormal), payload, randSeed);
}

[shader("miss")]
void Miss(inout PhotonEmitterPayload payload)
{
    payload.power = 0.0;
}
