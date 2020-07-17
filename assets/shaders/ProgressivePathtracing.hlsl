#include "RaytracingCommon.hlsli"
#include "ProceduralPrimitives.hlsli"

RWTexture2D<float4> gOutput : register(u0);

struct SimplePayload
{
    XMFLOAT4 colorAndDistance;
    UINT depth;
};

[shader("raygeneration")]
void RayGen()
{
    if (perFrameConstants.cameraParams.accumCount >= perFrameConstants.options.maxIterations) {
        return;
    }

    uint2 launchIndex = DispatchRaysIndex().xy;
    float2 dims = float2(DispatchRaysDimensions().xy);
    float2 d = (((launchIndex.xy + 0.5f) / dims.xy) * 2.f - 1.f);

    SimplePayload payload;
    payload.colorAndDistance = float4(0, 0, 0, 0);
    payload.depth = 0;

    float2 jitter = perFrameConstants.cameraParams.jitters * 30.0;

    RayDesc ray;
    ray.Origin = perFrameConstants.cameraParams.worldEyePos.xyz + float3(jitter.x, jitter.y, 0.0f);
    ray.Direction = normalize(d.x * perFrameConstants.cameraParams.U + (-d.y) * perFrameConstants.cameraParams.V + perFrameConstants.cameraParams.W).xyz;
    ray.TMin = 0;
    ray.TMax = RAY_MAX_T;

    TraceRay(SceneBVH, RAY_FLAG_FORCE_OPAQUE | RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 0, 0, ray, payload);

    float4 prevColor = gOutput[launchIndex];
    float4 curColor = float4(max(payload.colorAndDistance.rgb, 0.0), 1.0f);
    gOutput[launchIndex] = (perFrameConstants.cameraParams.accumCount * prevColor + curColor) / (perFrameConstants.cameraParams.accumCount + 1);
}

float3 shootSecondaryRay(float3 orig, float3 dir, float minT, uint currentDepth)
{
    if (currentDepth >= MAX_RADIANCE_RAY_DEPTH) {
        return float3(0.0, 0.0, 0.0);
    }

    RayDesc ray = { orig, minT, dir, RAY_MAX_T };

    SimplePayload payload;
    payload.colorAndDistance = float4(0, 0, 0, 0);
    payload.depth = currentDepth + 1;

    TraceRay(SceneBVH, 0, 0xFF, 0, 0, 0, ray, payload);
    return payload.colorAndDistance.rgb;
}

float3 evaluateIndirectDiffuse(float3 position, float3 normal, inout uint randSeed, uint currentDepth)
{
    float3 color = 0.0;
    const int rayCount = 1;

    for (int i = 0; i < rayCount; ++i) {
        if (perFrameConstants.options.cosineHemisphereSampling) {
            float3 sampleDir = getCosHemisphereSample1(randSeed, normal);
            // float NoL = saturate(dot(normal, sampleDir));
            // float pdf = NoL / M_PI;
            // color += shootSecondaryRay(position, sampleDir, RAY_EPSILON, currentDepth) * NoL / pdf;
            color += shootSecondaryRay(position, sampleDir, RAY_EPSILON, currentDepth) * M_PI; // term canceled
        } else {
            float3 sampleDir = getUniformHemisphereSample(randSeed, normal);
            float NoL = saturate(dot(normal, sampleDir));
            float pdf = 1.0 / (2.0 * M_PI);
            color += shootSecondaryRay(position, sampleDir, RAY_EPSILON, currentDepth) * NoL / pdf;
        }
    }

    return color / float(rayCount);
}

float3 shade(float3 position, float3 normal, uint currentDepth)
{
    // Set up random seed
    uint2 pixIdx = DispatchRaysIndex().xy;
    uint2 numPix = DispatchRaysDimensions().xy;
    uint randSeed = initRand(pixIdx.x + pixIdx.y * numPix.x, perFrameConstants.cameraParams.frameCount);

    float3 atten = materialParams.albedo.rgb;
    float3 directContrib = 0.0;
    float3 indirectContrib = 0.0;
    float emissiveAtten = dot(-WorldRayDirection().xyz, normal.xyz) < 0. ? 0. : 1.;

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
    }

    return emissiveAtten * materialParams.emissive.rgb * materialParams.emissive.a + materialParams.albedo.rgb * directContrib + atten * indirectContrib;
}

[shader("closesthit")]
void PrimaryClosestHit(inout SimplePayload payload, Attributes attrib)
{
    float3 vertPosition, vertNormal;
    interpolateVertexAttributes(attrib.bary, vertPosition, vertNormal);

    float3 color = shade(HitWorldPosition(), normalize(vertNormal), payload.depth);
    payload.colorAndDistance = float4(color, RayTCurrent());
}

[shader("closesthit")]
void PrimaryClosestHit_AABB(inout SimplePayload payload, in ProceduralPrimitiveAttributes attr)
{
    float3 color = shade(HitWorldPosition(), normalize(attr.normal), payload.depth);
    payload.colorAndDistance = float4(color, RayTCurrent());
}

[shader("miss")]
void PrimaryMiss(inout SimplePayload payload)
{
    payload.colorAndDistance = float4(0, 0, 0, -1); //float4(sampleEnvironment(), -1.0);
}

[shader("closesthit")]
void ShadowClosestHit(inout ShadowPayload payload, Attributes attrib)
{
    // no-op
}

[shader("anyhit")]
void ShadowAnyHit(inout ShadowPayload payload, Attributes attrib)
{
    // no-op
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload)
{
    payload.lightVisibility = 1.0;
}
