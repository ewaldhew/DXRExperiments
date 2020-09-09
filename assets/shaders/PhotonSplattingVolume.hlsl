#include "RaytracingCommon.hlsli"
#include "RaytracingShaderHelper.hlsli"

struct DummyAttr { uint padding; };

RWTexture2D<float4> ColorXYZAndDirectionX : register(u0);
RWTexture2D<float2> DirectionYZ : register(u1);

Texture2D<uint> photonDensity : register(t0, space1);
StructuredBuffer<Photon> volPhotonBuffer : register(t1, space1);

ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);

[shader("raygeneration")]
void RayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    float2 dims = float2(DispatchRaysDimensions().xy);
    float2 d = (((launchIndex.xy + 0.5f) / dims.xy) * 2.f - 1.f);
    uint randSeed = initRand(launchIndex.x + launchIndex.y * dims.x, perFrameConstants.cameraParams.frameCount);

    float2 jitter = perFrameConstants.cameraParams.jitters * 30.0;

    RayDesc ray;
    ray.Origin = perFrameConstants.cameraParams.worldEyePos.xyz + float3(jitter.x, jitter.y, 0.0f);
    ray.Direction = normalize(d.x * perFrameConstants.cameraParams.U + (-d.y) * perFrameConstants.cameraParams.V + perFrameConstants.cameraParams.W).xyz;
    ray.TMin = 0;
    ray.TMax = RAY_MAX_T;

    PhotonSplatPayload prd;

    //ray-AABB intersection to determine number of segments

    float3 t0, t1, tmin, tmax;
    float3 bbox_min = photonMapConsts.volumeBboxMin;
    float3 bbox_max = photonMapConsts.volumeBboxMax;
    t0 = (bbox_max - ray.Origin) / ray.Direction;
    t1 = (bbox_min - ray.Origin) / ray.Direction;
    tmax = max(t0, t1);
    tmin = min(t0, t1);
    float tenter = max(0.f, max(tmin.x, max(tmin.y, tmin.z)));
    float texit = min(tmax.x, min(tmax.y, tmax.z));

    const float fixed_radius = photonMapConsts.volumeSplatPhotonSize;
    float slab_spacing = PARTICLE_BUFFER_SIZE * photonMapConsts.particlesPerSlab * fixed_radius;

    float3 direction = 0.f;
    float3 result = 0.f;
    float result_alpha = 0.f;

    if (tenter < texit)
    {
        float tbuf = 0.f;

        //for each segment,
        //  traverse the BVH (collect deep samples in prd.particles),
        //  sort,
        //  integrate.

        while(tbuf < texit && result_alpha < 0.97f)
        {
            prd.tail = 0;
            ray.TMin = max(tenter, tbuf);
            ray.TMax = min(texit, tbuf + slab_spacing);

            if (ray.TMax > tenter)    //doing this will keep rays more coherent
            {
                TraceRay(SceneBVH, 0, 0xFF, 0, 0, 0, ray, prd);

                //sort() in RT Gems pseudocode
                int N = prd.tail;
                int i;

                //bubble sort
                for (i=0; i<N; i++) {
                    for (int j=0; j < N-i-1; j++)
                    {
                        const float2 tmp = prd.particles[i];
                        if (tmp.x < prd.particles[j].x) {
                            prd.particles[i] = prd.particles[j];
                            prd.particles[j] = tmp;
                        }
                    }
                }

                //integrate depth-sorted list of particles
                for (i=0; i<prd.tail; i++) {
                    float trbf = prd.particles[i].x;
                    uint idx = uint(prd.particles[i].y);

                    Photon photon = volPhotonBuffer[idx];

                    // TODO: blending
                    float4 color_sample = float4(photon.power, 1.0f);
                    direction = photon.direction;

                    float alpha = color_sample.a;
                    float alpha_1msa = alpha * (1.0 - result_alpha);
                    result += color_sample.rgb * alpha_1msa;
                    result_alpha += alpha_1msa;
                }
            }

            tbuf += slab_spacing;
        }
    }

    float total_power = dot(result.xyz, float3(1.0f, 1.0f, 1.0f));
    float3 weighted_direction = total_power * direction;

    ColorXYZAndDirectionX[launchIndex] += float4(result, weighted_direction.x);
    DirectionYZ[launchIndex] += weighted_direction.yz;
}

uint PhotonIndex()
{
    switch (photonMapConsts.photonGeometryBuildStrategy) {
    case 0: return PrimitiveIndex();
    case 1: return InstanceIndex();
    default: return 0;
    }
}

[shader("intersection")]
void ParticleIntersect()
{
    const float3 pos = volPhotonBuffer[PhotonIndex()].position.xyz;
    const float t = length(pos - WorldRayOrigin());
    const float3 samplePos = WorldRayOrigin() + WorldRayDirection() * t;

    if (length(pos - samplePos) < photonMapConsts.volumeSplatPhotonSize && IsInRange(t, RayTMin(), RayTCurrent()))
    {
        DummyAttr attr;
        ReportHit(t, /*hitKind*/ 0, attr);
    }
}

[shader("anyhit")]
void AnyHit(inout PhotonSplatPayload payload, in DummyAttr attr)
{
    if (payload.tail < PARTICLE_BUFFER_SIZE)
    {
        payload.particles[payload.tail] = float2(RayTCurrent(), PhotonIndex());
        payload.tail++;
        IgnoreHit();
    }
}

[shader("closesthit")]
void ClosestHit(inout PhotonSplatPayload payload, in DummyAttr attr)
{
}

[shader("miss")]
void Miss(inout PhotonSplatPayload payload)
{
}
