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
    float3 power; // last component is scaling factor
    // Ray length
    float distTravelled;
    // Bounce count
    uint bounce;

    // Outgoing payload
    float3 position;
    float3 direction;
};

// Global root signature
StructuredBuffer<Photon> photonSeed : register(t3);

RWBuffer<uint> gCounters : register(u0);
RWStructuredBuffer<Photon> gPhotonMapSurface : register(u1);
RWStructuredBuffer<Photon> gPhotonMapVolume : register(u2);

RWTexture2D<uint> gPhotonDensityMap : register(u3);

RWStructuredBuffer<PhotonAABB> gVolumePhotonAabb : register(u4);
RWBuffer<float4> gVolumePhotonPos : register(u5);

ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);

// Local root signature
cbuffer Material : register(b2, space1)
{
    uint materialIndex;
}

void validate_and_add_photon(float3 normal, float3 position, float3 power, float3 in_direction, float t, uint map_idx)
{
    if (any(power) && isInCameraFrustum(position))
    {
        float2 offsetFromBottomRight = (unproject(position) + 1.f) / 2.f;
        uint2 tile = uint2((offsetFromBottomRight * float2(photonMapConsts.numTiles.xy)).xy);

        // Offset in the photon buffer and the indirect argument
        uint photon_index;
        InterlockedAdd(gCounters[map_idx], 1, photon_index);

        // Photon is packed and stored with correct offset.
        Photon stored_photon;
        stored_photon.position = position;
        stored_photon.power = power;
        stored_photon.direction = unitvec_to_spherical(in_direction);
        stored_photon.normal = unitvec_to_spherical(normal);
        stored_photon.distTravelled = t;
        stored_photon.materialIndex = materialIndex;

        switch (map_idx) {
        case PhotonMapID::Surface:
            gPhotonMapSurface[photon_index] = stored_photon;
            break;
        case PhotonMapID::Volume:
            gPhotonMapVolume[photon_index] = stored_photon;
            gVolumePhotonPos[photon_index] = float4(stored_photon.position, 1);
            PhotonAABB aabb = {
                stored_photon.position - photonMapConsts.volumeSplatPhotonSize,
                stored_photon.position + photonMapConsts.volumeSplatPhotonSize,
                0.0f, 0.0f
            };
            gVolumePhotonAabb[photon_index] = aabb;
            break;
        }

        // Tile-based photon density estimation
        InterlockedAdd(gPhotonDensityMap[tile.xy], 1);
    }
}

[shader("raygeneration")]
void RayGen()
{
    // First, we read the initial sample from the RSM.
    uint sampleIndex = DispatchRaysIndex().x;

    PhotonPayload payload;
    payload.random = photonSeed[sampleIndex].randSeed;
    payload.power = photonSeed[sampleIndex].power;
    payload.distTravelled = photonSeed[sampleIndex].distTravelled;
    payload.bounce = 0;
    payload.position = photonSeed[sampleIndex].position;
    payload.direction = photonSeed[sampleIndex].direction.xyz;

    RayDesc ray;
    ray.Origin = photonSeed[sampleIndex].position;
    ray.Direction = photonSeed[sampleIndex].direction.xyz;
    ray.TMin = RAY_EPSILON;
    ray.TMax = RAY_MAX_T;

    // Before starting the trace, check if photon was
    // spawned inside a volume and trace it to the edge first
    {
        TraceRay(SceneBVH, 0, MaterialSceneFlags::Volume, 2, 0, 2, ray, payload);

        ray.Origin = payload.position;
        ray.Direction = payload.direction;
    }

    while (payload.bounce < MAX_PHOTON_DEPTH - 1) {
        uint bounce = payload.bounce;

        TraceRay(SceneBVH, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 0, 0, ray, payload);

        if (payload.bounce == bounce) {
            // prevent infinite loop
            payload.bounce = MAX_PHOTON_DEPTH;
        }

        ray.Origin = payload.position;
        ray.Direction = payload.direction;
    }
}

bool russian_roulette(float3 in_power, float3 in_direction, float3 normal, inout float3 position, inout float dist, inout uint randSeed,
                      out float4 out_power, out float3 out_direction)
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
    default:
        break;
    }

    if (mat.type == MaterialType::ParticipatingMedia) {
        float throughput = 1.0;
        uint numInteractions = 0;
        float3 prevPosition = position;
        float3 params; // x - extinction, y - scattering
        float3 rayDir = WorldRayDirection();
        float t;
        while (evaluateVolumeInteraction(randSeed, position, rayDir, t, params, 1))
        {
            if (numInteractions++ > MAX_VOLUME_INTERACTIONS) {
                break;
            }

            // attenuate by albedo = scattering / extinction
            throughput *= params.y / params.x;

            // Russian roulette absorption
            if (throughput < 0.2) {
                if (nextRand(randSeed) > throughput * 5.0) {
                    throughput = 0.0;
                    break;
                }
                throughput = 0.2;
            }

            float3 rayDirPrev = rayDir;

            // Sample the phase function
            { // isotropic
                rayDir = getUniformSphereSample(randSeed);
            }

            dist += t;
            float3 stored_power = in_power * throughput;
            validate_and_add_photon(rayDir, prevPosition, stored_power, rayDirPrev, dist, PhotonMapID::Volume);

            prevPosition = position;
        }

        if (!any(throughput)) {
            return false;
        }

        rayDir = normalize(position - prevPosition);
        position = prevPosition;
        out_direction = rayDir;
        out_factor = throughput;

        float3 r = out_factor;
        float3 rp = r * in_power;
        float3 p = in_power;

        float q = max(rp.r, max(rp.g, rp.b)) / max(p.r, max(p.g, p.b)); // termination probability

        out_power = float4(rp, q);

        bool keep_going = nextRand(randSeed) < q;

        return keep_going;

    } else {
        float3 r = out_factor * mat.albedo.rgb * mat.albedo.a;
        float3 rp = r * in_power;
        float3 p = in_power;

        float q = max(rp.r, max(rp.g, rp.b)) / max(p.r, max(p.g, p.b)); // termination probability

        out_power = float4(rp, q);

        float3 stored_power = out_power.rgb;
        validate_and_add_photon(normal, position, stored_power, in_direction, dist, PhotonMapID::Surface);

        bool keep_going = nextRand(randSeed) < q;

        return keep_going;
    }

}

void handle_hit(float3 position, float3 normal, inout PhotonPayload payload)
{
    float3 incoming_power = payload.power.rgb;
    float3 ray_direction = WorldRayDirection();
    uint rand = payload.random;
    float4 outgoing_power = .0f;
    float3 outgoing_direction = .0f;
    float3 pos = position;
    float dist = payload.distTravelled + RayTCurrent();
    bool keep_going = russian_roulette(incoming_power, ray_direction, normal, pos, dist, rand,
                                       outgoing_power, outgoing_direction);

    payload.position = pos;
    payload.direction = outgoing_direction;
    payload.random = rand;
    payload.power = outgoing_power.rgb / outgoing_power.a;
    payload.distTravelled = dist;
    payload.bounce = keep_going ? payload.bounce + 1 : MAX_PHOTON_DEPTH;
}

[shader("closesthit")]
void StartingVolumeHit(inout PhotonPayload payload, in Attributes attrib)
{
    float3 vertPosition, vertNormal;
    interpolateVertexAttributes(attrib.bary, vertPosition, vertNormal);

    if (exitingVolume(normalize(vertNormal), WorldRayDirection())) {
        handle_hit(WorldRayOrigin(), WorldRayDirection(), payload);
    }
}

[shader("closesthit")]
void StartingVolumeHit_AABB(inout PhotonPayload payload, in ProceduralPrimitiveAttributes attr)
{
    if (exitingVolume(normalize(attr.normal), WorldRayDirection())) {
        handle_hit(WorldRayOrigin(), WorldRayDirection(), payload);
    }
}

[shader("miss")]
void StartingVolumeMiss(inout PhotonPayload payload)
{
    // no-op
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
