#include "RaytracingCommon.hlsli"
#include "ProceduralPrimitives.hlsli"

#define MAX_PHOTON_DEPTH 4

struct PhotonPayload
{
    // RNG state
    uint2 random;
    // Packed photon power
    //uint power;
    float3 power;
    // Ray length
    float distTravelled;
    // Bounce count
    uint bounce;
};

// Global root signature
StructuredBuffer<Photon> photonSeed : register(t3);

RWStructuredBuffer<Photon> gOutput : register(u0);
RWTexture2D<uint> gPhotonDensityMap : register(u1);

ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);

[shader("raygeneration")]
void RayGen()
{
    // First, we read the initial sample from the RSM.
    uint sampleIndex = DispatchRaysIndex().x;

    PhotonPayload payload;
    payload.random.x = photonSeed[sampleIndex].randSeed;
    payload.random.y = 0;
    payload.power = photonSeed[sampleIndex].power;
    payload.distTravelled = photonSeed[sampleIndex].distTravelled;
    payload.bounce = 0;

    RayDesc ray;
    ray.Origin = photonSeed[sampleIndex].position;
    ray.Direction = photonSeed[sampleIndex].direction.xyz;
    ray.TMin = RAY_EPSILON;
    ray.TMax = RAY_MAX_T;

    TraceRay(SceneBVH, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, ray, payload);
}

bool russian_roulette(float3 power, float3 direction, float3 normal, uint2 rand,
                      inout float3 out_power, inout float3 out_direction, inout float3 stored_power)
{/*
    // Set up random seed
    uint2 pixIdx = DispatchRaysIndex().xy;
    uint2 numPix = DispatchRaysDimensions().xy;
    uint randSeed = initRand(pixIdx.x + pixIdx.y * numPix.x, perFrameConstants.cameraParams.frameCount);

    float3 atten = materialParams.albedo.rgb;
    float3 directContrib = 0.0;
    float3 indirectContrib = 0.0;

    // indirect light - sample scattering direction to evaluate secondary ray
    switch (materialParams.type)
    {
    case 0: { // lambertian
        // Calculate indirect diffuse
        if (!perFrameConstants.options.noIndirectDiffuse) {
            indirectContrib = evaluateIndirectDiffuse(position, normal, randSeed, currentDepth) / M_PI;
        }
        break;
    }
    case 1: { // metal
        float exponent = exp((1.0 - materialParams.roughness) * 12.0);
        float pdf;
        float brdf;
        float3 mirrorDir = reflect(WorldRayDirection(), normal);
        float3 sampleDir = samplePhongLobe(randSeed, mirrorDir, exponent, pdf, brdf);
        if (dot(sampleDir, normal) > 0) {
            float3 reflectionColor = shootSecondaryRay(position, sampleDir, RAY_EPSILON, currentDepth);
            indirectContrib = reflectionColor * brdf / pdf;
        }
        break;
    }
    case 2: { // dielectric
        float3 refractDir;
        float3 reflectProb;
        bool refracted = refract(refractDir, WorldRayDirection(), normal, materialParams.IoR);
        if (refracted) {
            float f0 = ((materialParams.IoR-1)*(materialParams.IoR-1) / (materialParams.IoR+1)*(materialParams.IoR+1));
            reflectProb = FresnelReflectanceSchlick(WorldRayDirection(), normal, f0);
        } else {
            reflectProb = 1.0;
        }

        uint randIndex = floor(nextRand(randSeed) * 2.9999);
        if (nextRand(randSeed) < reflectProb[randIndex]) {
            float3 mirrorDir = reflect(WorldRayDirection(), normal);
            indirectContrib = shootSecondaryRay(position, mirrorDir, RAY_EPSILON, currentDepth);
        } else {
            indirectContrib = shootSecondaryRay(position, refractDir, RAY_EPSILON, currentDepth);
        }
        break;
    }
    }*/
    return false;
}

void validate_and_add_photon(float3 normal, float3 position, float3 power, float3 in_direction, float t)
{
    if (isInCameraFrustum(position) && isInViewDirection(normal)) {
        float2 offsetFromBottomRight = (unproject(position) + 1.f) / 2.f;
        uint2 tile = uint2((offsetFromBottomRight * float2(photonMapConsts.numTiles.xy)).xy);

        // Offset in the photon buffer and the indirect argument
        uint photon_index = gOutput.IncrementCounter();

        // Photon is packed and stored with correct offset.
        Photon stored_photon;
        stored_photon.position = position;
        stored_photon.power = power;
        stored_photon.direction = unitvec_to_spherical(in_direction);
        stored_photon.normal = unitvec_to_spherical(normal);
        stored_photon.distTravelled = t;
        gOutput[photon_index] = stored_photon;

        // Tile-based photon density estimation
        InterlockedAdd(gPhotonDensityMap[tile.xy], 1);
    }
}

void handle_hit(float3 position, float3 normal, PhotonPayload payload)
{
    float3 incoming_power = payload.power;
    float3 ray_direction = WorldRayDirection();
    uint2 random = payload.random;
    float3 outgoing_power = .0f;
    float3 outgoing_direction = .0f;
    float3 stored_power = .0f;
    bool keep_going = russian_roulette(incoming_power, ray_direction, normal, random,
                                       outgoing_power, outgoing_direction, stored_power);

    validate_and_add_photon(normal, position, stored_power, ray_direction, payload.distTravelled);

    if (keep_going && payload.bounce < MAX_PHOTON_DEPTH - 1) {
        RayDesc ray = { position, RAY_EPSILON, outgoing_direction, RAY_MAX_T };

        PhotonPayload next;
        next.random = random;
        next.power = outgoing_power;
        next.distTravelled = payload.distTravelled + RayTCurrent();
        next.bounce = payload.bounce + 1;

        TraceRay(SceneBVH, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, ray, next);
    }
}

[shader("closesthit")]
void ClosestHit(inout PhotonPayload payload, in Attributes attrib)
{
    // Load surface attributes for the hit.
    float3 vertPosition, vertNormal;
    interpolateVertexAttributes(attrib.bary, vertPosition, vertNormal);

    handle_hit(HitWorldPosition(), normalize(vertNormal), payload);
}

[shader("closesthit")]
void ClosestHit_AABB(inout PhotonPayload payload, in ProceduralPrimitiveAttributes attrib)
{
    handle_hit(HitWorldPosition(), normalize(attrib.normal), payload);
}

[shader("miss")]
void Miss(inout PhotonPayload payload)
{
    // no-op
}
