#define HLSL
#include "RasterHlslCompat.h"
#include "RaytracingUtils.hlsli"
#undef RASTER_PIPELINE
#include "ParticipatingMediaUtil.hlsli"

ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);

Texture2D<float> gbufferDepth : register(t0, space2);
Texture2D<float> gbufferVolumeMask : register(t1, space2);
Texture3D<float4> voxColorAndCount : register(t2, space2);
Texture3D<float4> voxDirectionAndMatId : register(t3, space2);

RWTexture2D<float4> photonSplatColorXYZDirX : register(u0);
RWTexture2D<float2> photonSplatDirYZ : register(u1);

SamplerState lightSampler : register(s0, space1);
SamplerState linearSampler : register(s1, space1);

struct unpacked_photon
{
    float3 position;
    float3 power;
    float3 direction;
    float3 normal;
    float distTravelled;
    uint materialIndex;
};

float3 raymarch(float3 origin, float3 dir, float2 screen_pos, inout uint randSeed)
{
    float z_factor = dot(dir, normalize(perFrameConstants.cameraParams.W.xyz));
    float gbuffer_linear_depth = gbufferDepth.SampleLevel(linearSampler, screen_pos, 0.0);

    uint3 tex_size;
    voxColorAndCount.GetDimensions(tex_size.x, tex_size.y, tex_size.z);
    float3 vol_bbox_size = photonMapConsts.volumeBboxMax.xyz - photonMapConsts.volumeBboxMin.xyz;
    float3 cell_size = vol_bbox_size / tex_size;
    float step = min(cell_size.x, min(cell_size.y, cell_size.z));

    float3 t0, t1, tmin, tmax;
    float3 bbox_min = photonMapConsts.volumeBboxMin;
    float3 bbox_max = photonMapConsts.volumeBboxMax;
    t0 = (bbox_max - origin) / dir;
    t1 = (bbox_min - origin) / dir;
    tmax = max(t0, t1);
    tmin = min(t0, t1);
    float tenter = max(0.f, max(tmin.x, max(tmin.y, tmin.z))) - 0.1f;
    float texit = min(min(tmax.x, min(tmax.y, tmax.z)), gbuffer_linear_depth / z_factor);

    float3 result = 0.f;
    float result_alpha = 1.f;

    if (tenter < texit) {
        float t = tenter;
        while (t <  gbuffer_linear_depth / z_factor && result_alpha > 0.03f) {
            float3 curr_pos = origin + dir * t;
            float3 tex_coords = (curr_pos - photonMapConsts.volumeBboxMin.xyz) / vol_bbox_size;

            float4 color_count = voxColorAndCount.SampleLevel(linearSampler, tex_coords, 1.0);
            float3 color = color_count.rgb;
            uint count = uint(round(color_count.w));
            float4 light_dir_mat_id = voxDirectionAndMatId.SampleLevel(linearSampler, tex_coords, 0.0);
            float3 light_dir = normalize(light_dir_mat_id.xyz);
            uint mat_id = uint(round(light_dir_mat_id.w));

            float3 curr_pos_obj = mul(photonMapConsts.worldToObjMatrix, float4(curr_pos, 1.0f)).xyz;
            MaterialParams mat = matParams[mat_id];
            collectMaterialParams1(mat, curr_pos_obj);
            VolumeParams vol = getVolumeParams(mat);
            float3 emission = vol.emission;
            float absorption = vol.absorption;
            float scattering = vol.scattering;
            float extinction = absorption + scattering;
            float albedo = scattering / extinction;
            float phase_factor = evalPhaseFuncPdf(vol.phase_func_type, light_dir, dir);

            float3 power = color * phase_factor;
            float alpha = exp(-extinction * step / 10);
            float3 sample_color = (absorption * emission + albedo * power) *step;
            result += sample_color * result_alpha / max(count, 1);
            result_alpha *= alpha;

            t += step / z_factor * nextRand(randSeed);
        }
    }

    return result * result_alpha;
}

[numthreads(8, 8, 1)]
void main( uint3 tid : SV_DispatchThreadID )
{
    uint2 Pos = tid.xy;
    float2 Tex = Pos / float2(perFrameConstants.cameraParams.vpSize.xy);
    float2 d = Tex.xy * 2.f - 1.f;
    float3 viewDir = normalize(d.x * perFrameConstants.cameraParams.U + (-d.y) * perFrameConstants.cameraParams.V + perFrameConstants.cameraParams.W).xyz;
    uint randSeed = initRand(Pos.x + Pos.x * Pos.y, perFrameConstants.cameraParams.frameCount);

    float3 volumeColor = gbufferVolumeMask.Load(int3(Pos, 0)) * raymarch(perFrameConstants.cameraParams.worldEyePos.xyz, viewDir, Tex, randSeed);

    photonSplatColorXYZDirX[Pos] += float4(volumeColor, viewDir.x);
    photonSplatDirYZ[Pos] += viewDir.yz;
}
