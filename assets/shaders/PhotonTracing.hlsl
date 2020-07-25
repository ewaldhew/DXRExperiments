#include "RaytracingCommon.hlsli"
#include "ProceduralPrimitives.hlsli"
#include "ParticipatingMedia.hlsli"

#define MAX_PHOTON_DEPTH 8

struct PhotonPayload
{
    // RNG state
    uint random;
    // Packed photon power
    //uint power;
    float4 power; // last component is scaling factor
    // Ray length
    float distTravelled;
    // Bounce count
    uint bounce;

    float3 normal;

    // Outgoing payload
    float3 position;
    float3 direction;
};

// Global root signature
StructuredBuffer<Photon> photonSeed : register(t3);

RWStructuredBuffer<Photon> gOutput : register(u0);
RWTexture2D<float4> gDebug : register(u9);
RWTexture2D<uint> gPhotonDensityMap : register(u1);

ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);


void validate_and_add_photon(float3 normal, float3 position, float3 power, float3 in_direction, float t)
{
    if (any(power) && isInCameraFrustum(position))
    {
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

        //
        offsetFromBottomRight = (unproject(position) + 1.f) / 2.f;
        tile = uint2((offsetFromBottomRight * float2(photonMapConsts.vpSize.xy)).xy);
        gDebug[tile] = float4(power * 0.01, 1.0);
        //gDebug[tile] = float4(1,0,1, 1.0);
        //
    }
}

[shader("raygeneration")]
void RayGen()
{
    // First, we read the initial sample from the RSM.
    uint sampleIndex = DispatchRaysIndex().x;

    PhotonPayload payload;
    payload.random = photonSeed[sampleIndex].randSeed;
    payload.power = float4(photonSeed[sampleIndex].power, 1.0);
    payload.distTravelled = photonSeed[sampleIndex].distTravelled;
    payload.bounce = 0;

    RayDesc ray;
    ray.Origin = photonSeed[sampleIndex].position;
    ray.Direction = photonSeed[sampleIndex].direction.xyz;
    ray.TMin = RAY_EPSILON;
    ray.TMax = RAY_MAX_T;

    while (payload.bounce < MAX_PHOTON_DEPTH - 1) {
        uint bounce = payload.bounce;
        float3 in_power = payload.power.rgb;
        float3 in_direction = ray.Direction;

        TraceRay(SceneBVH, RAY_FLAG_FORCE_OPAQUE, 0xFF, 0, 0, 0, ray, payload);

        float3 stored_power = payload.power.rgb;
        validate_and_add_photon(payload.normal, payload.position, stored_power, in_direction, payload.distTravelled);

        if (payload.bounce == bounce) {
            // prevent infinite loop
            payload.bounce = MAX_PHOTON_DEPTH;
        }
        payload.power /= payload.power.a;

        ray.Origin = payload.position;
        ray.Direction = payload.direction;
    }
}

bool russian_roulette(float3 in_power, float3 in_direction, float3 normal, inout float3 position, inout uint randSeed,
                      inout float4 out_power, inout float3 out_direction)
{
    MaterialParams mat = materialParams;
    collectMaterialParams(mat);

    float out_factor = 0.0;

    // sample brdf for outgoing direction and attenuation
    switch (mat.type)
    {
    case MaterialType::Diffuse:
    case MaterialType::DiffuseTexture: { // lambertian
        if (!perFrameConstants.options.noIndirectDiffuse) {
            if (perFrameConstants.options.cosineHemisphereSampling) {
                out_direction = getCosHemisphereSample1(randSeed, normal);
                // float NoL = saturate(dot(normal, sampleDir));
                // float pdf = NoL / M_PI;
                // float brdf = 1 / M_PI;
                out_factor = 1.0; //= NoL * brdf / pdf;
            }
            else {
                out_direction = getUniformHemisphereSample(randSeed, normal);
                float NoL = saturate(dot(normal, out_direction));
                // float pdf = 1.0 / (2.0 * M_PI);
                // float brdf = 1 / M_PI;
                out_factor = NoL * 2.0;
            }
        }
        break;
    }
    case MaterialType::Glossy: { // metal
        float exponent = exp((1.0 - mat.roughness) * 12.0);
        float pdf;
        float brdf;
        float3 mirrorDir = reflect(WorldRayDirection(), normal);
        float3 sampleDir = samplePhongLobe(randSeed, mirrorDir, exponent, pdf, brdf);
        if (dot(sampleDir, normal) > 0) {
            out_factor = brdf / pdf;
            out_direction = sampleDir;
        }
        break;
    }
    case MaterialType::Glass: { // dielectric
        float3 refractDir;
        float3 reflectProb;
        bool refracted = refract(refractDir, WorldRayDirection(), normal, mat.IoR);
        if (refracted) {
            float f0 = ((mat.IoR-1)*(mat.IoR-1) / (mat.IoR+1)*(mat.IoR+1));
            reflectProb = FresnelReflectanceSchlick(WorldRayDirection(), normal, f0);
        } else {
            reflectProb = 1.0;
        }

        uint randIndex = floor(nextRand(randSeed) * 2.9999);
        if (nextRand(randSeed) < reflectProb[randIndex]) {
            float3 mirrorDir = reflect(WorldRayDirection(), normal);
            out_direction = mirrorDir;
        } else {
            out_direction = refractDir;
        }
        out_factor = 1.0;
        break;
    }
    case MaterialType::ParticipatingMedia: {
        float throughput = 1.0;
        float3 prevPosition = position;
        float3 params; // x - extinction, y - scattering
        float3 rayDir = WorldRayDirection();

        bool inVolume = evaluateVolumeInteraction(randSeed, position, rayDir, params, 1);

        // attenuate by albedo = scattering / extinction
        throughput *= params.y / params.x;

        // Russian roulette absorption
        if (throughput < 0.2) {
            if (nextRand(randSeed) > throughput * 5.0) {
                throughput = 0.0;
            }
            throughput = 0.2;
        }

        if (inVolume) {
            // Sample the phase function
            { // isotropic
                rayDir = getUniformSphereSample(randSeed);
            }
        } else {
            rayDir = normalize(position - prevPosition);
            position = prevPosition;
        }

        out_direction = rayDir;
        out_factor = 0;// throughput;
        break;
    }
    }

    float3 r = out_factor * mat.albedo.rgb * mat.albedo.a;
    float3 rp = r * in_power;
    float3 p = in_power;

    float q = max(rp.r, max(rp.g, rp.b)) / max(p.r, max(p.g, p.b)); // termination probability

    out_power = float4(rp, q);

    bool keep_going = nextRand(randSeed) < q;

    return keep_going;
}

void handle_hit(float3 position, float3 normal, inout PhotonPayload payload)
{
    float3 incoming_power = payload.power.rgb;
    float3 ray_direction = WorldRayDirection();
    uint rand = payload.random;
    float4 outgoing_power = .0f;
    float3 outgoing_direction = .0f;
    float3 pos = position;
    bool keep_going = russian_roulette(incoming_power, ray_direction, normal, pos, rand,
                                       outgoing_power, outgoing_direction);

    payload.position = pos;
    payload.normal = normal;
    payload.direction = outgoing_direction;
    payload.random = rand;
    payload.power = outgoing_power;
    payload.distTravelled = payload.distTravelled + RayTCurrent();
    payload.bounce = keep_going ? payload.bounce + 1 : MAX_PHOTON_DEPTH;
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
    payload.bounce = MAX_PHOTON_DEPTH;
}
