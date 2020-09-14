#include "RaytracingCommon.hlsli"
#include "RaytracingShaderHelper.hlsli"

struct DummyAttr { uint padding; };

RWTexture2D<float4> ColorXYZAndDirectionX : register(u0);
RWTexture2D<float2> DirectionYZ : register(u1);

Texture2D<uint> photonDensity : register(t0, space1);
StructuredBuffer<Photon> volPhotonBuffer : register(t1, space1);

ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);

// Simplified version of the one in PhotonSplatting.vs
float kernel_size(float3 n, float3 light, float pos_z, float ray_length)
{
    // Tile-based culling as photon density estimation
    uint2 launchIndex = DispatchRaysIndex().xy;
    float2 dims = float2(DispatchRaysDimensions().xy);
    float2 posScreen = launchIndex / dims;
    uint2 tile = uint2(posScreen.x * photonMapConsts.numTiles.x, (1.0 - posScreen.y) * photonMapConsts.numTiles.y);
    int n_p = max(1, photonDensity.Load(int3(tile, 0)));

    // Equation 24.5
    float a_view = pos_z * pos_z * photonMapConsts.tileAreaConstant;
    float r = sqrt(a_view / (M_PI * n_p));

    // Equation 24.6
    float s_d = clamp(r, photonMapConsts.kernelScaleMin, photonMapConsts.kernelScaleMax) * photonMapConsts.uniformScaleStrength;

    // Equation 24.2
    float s_l = clamp(ray_length / photonMapConsts.maxRayLength, .1f, 1.0f);
    float scaling_uniform = s_d * s_l;

    float3 l = normalize(light);
    float cos_theta = saturate(dot(n, l));

    // Equation 24.7
    float light_shaping_scale = min(1.0f/cos_theta, photonMapConsts.maxLightShapingScale);

    float min_scale = photonMapConsts.kernelScaleMin * .1f;
    float min_area = min_scale * min_scale * 1.0f;
    float ellipse_area = scaling_uniform  * scaling_uniform * light_shaping_scale;

    return ellipse_area / min_area;
}


[shader("raygeneration")]
void RayGen()
{
    uint2 launchIndex = DispatchRaysIndex().xy;
    float2 dims = float2(DispatchRaysDimensions().xy);
    float2 d = (((launchIndex.xy + 0.5f) / dims.xy) * 2.f - 1.f);
    uint randSeed = initRand(launchIndex.x + launchIndex.y * dims.x, perFrameConstants.cameraParams.frameCount);

    float2 jitter = 0.0f;perFrameConstants.cameraParams.jitters * 30.0;

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
                //for (i=0; i<N; i++) {
                //    for (int j=0; j < N-i-1; j++)
                //    {
                //        const float2 tmp = prd.particles[i];
                //        if (tmp.x < prd.particles[j].x) {
                //            prd.particles[i] = prd.particles[j];
                //            prd.particles[j] = tmp;
                //        }
                //    }
                //}
                uint minidx = 2147483647;
                for (i=0;i<N;i++){
                    if (minidx > prd.particles[i].y) {
                        minidx = prd.particles[i].y;
                    }
                }

                //integrate depth-sorted list of particles
                /*for (i=0; i<prd.tail; i++)*/i=minidx+1;if(prd.tail) {
                    float trbf = prd.particles[i].x;
                    uint idx = uint(prd.particles[i].y);

                    Photon photon = volPhotonBuffer[idx];

                    float kernel_scale = kernel_size(photon.normal, -photon.direction, photon.position.z, photon.distTravelled);

                    // TODO: blending
                    //float4 color_sample = float4(photon.power, 1.0f);
                    //direction = -photon.direction;

                    float3 power = float3(idx, 0,0);//photon.power / kernel_scale;
                    float3 direction = -photon.direction;
                    float total_power = dot(power.xyz, float3(1.0f, 1.0f, 1.0f));
                    float3 weighted_direction = total_power * direction;

                    ColorXYZAndDirectionX[launchIndex] += float4(power, weighted_direction.x);
                    DirectionYZ[launchIndex] += weighted_direction.yz;

                    //float alpha = color_sample.a;
                    //float alpha_1msa = alpha * (1.0 - result_alpha);
                    //result += color_sample.rgb * alpha_1msa;
                    //result_alpha += alpha_1msa;
                    break;
                }
            }

            tbuf += slab_spacing;
        }
    }

    //float total_power = dot(result.xyz, float3(1.0f, 1.0f, 1.0f));
    //float3 weighted_direction = total_power * direction;

    //ColorXYZAndDirectionX[launchIndex] += float4(result, weighted_direction.x);
    //DirectionYZ[launchIndex] += weighted_direction.yz;
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
