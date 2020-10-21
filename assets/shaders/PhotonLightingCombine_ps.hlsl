#include "RasterCommon.hlsli"
#include "ParticipatingMediaUtil.hlsli"

Texture2D<float4> gbufferNormals : register(t0, space1);

Texture2D<float4> photonSplatColorXYZDirX : register(t1, space1);
Texture2D<float2> photonSplatDirYZ : register(t2, space1);

ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);
Texture2D<float> gbufferDepth : register(t0, space2);
Texture2D<float> gbufferVolumeDepth : register(t1, space2);
Texture3D<float4> voxColorAndCount : register(t2, space2);
Texture3D<float4> voxDirectionAndMatId : register(t3, space2);

SamplerState lightSampler : register(s0, space1);
SamplerState linearSampler : register(s1, space1);

/*
float evaluateBrdf(float3 InDir, float3 OutDir, float3 N)
{
    MaterialParams mat = materialParams;

    switch (mat.type)
    {
    case MaterialType::Diffuse:
    case MaterialType::DiffuseTexture: { // lambertian
        float NoL = saturate(dot(N, InDir));
        return NoL / M_PI;
    }
    case MaterialType::Glossy: { // metal
        float exponent = exp((1.0 - mat.roughness) * 12.0);
        float3 mirrorDir = reflect(InDir, N);
        float cosTheta = dot(mirrorDir, OutDir);
        return pow(cosTheta, exponent);
    }
    case MaterialType::Glass: { // dielectric
        float3 refractDir;
        bool refracted = refract(refractDir, InDir, N, mat.IoR);
        if (refracted) {
            float f0 = ((mat.IoR-1)*(mat.IoR-1) / (mat.IoR+1)*(mat.IoR+1));
            return FresnelReflectanceSchlick(InDir, N, f0);
        } else {
            return 1.0;
        }
    }
    default:
        return 1.0;
    }
}*/

float4 raymarch(float3 origin, float3 dir, float2 screen_pos, float vol_linear_depth, inout uint randSeed)
{
    float z_factor = dot(dir, normalize(perFrameConstants.cameraParams.W.xyz));
    float gbuffer_linear_depth = gbufferDepth.Sample(linearSampler, screen_pos);

    uint3 tex_size;
    voxColorAndCount.GetDimensions(tex_size.x, tex_size.y, tex_size.z);
    float3 vol_bbox_size = photonMapConsts.volumeBboxMax.xyz - photonMapConsts.volumeBboxMin.xyz;
    float3 cell_size = vol_bbox_size / tex_size;
    float step = min(cell_size.x, min(cell_size.y, cell_size.z)) * 1.7f;

    float3 t0, t1, tmin, tmax;
    float3 bbox_min = photonMapConsts.volumeBboxMin;
    float3 bbox_max = photonMapConsts.volumeBboxMax;
    t0 = (bbox_max - origin) / dir;
    t1 = (bbox_min - origin) / dir;
    tmax = max(t0, t1);
    tmin = min(t0, t1);
    float tstart = vol_linear_depth / z_factor;
    float tenter = max(max(0.f, max(tmin.x, max(tmin.y, tmin.z))), tstart);
    float texit = min(min(tmax.x, min(tmax.y, tmax.z)), gbuffer_linear_depth / z_factor);

    float3 result = 0.f;
    float result_alpha = .05f;

    if (tenter < texit) {
        float t = tenter;
        while (t <  texit && result_alpha > 0.03f) {
            float3 curr_pos = origin + dir * t;
            float3 tex_coords = (curr_pos - photonMapConsts.volumeBboxMin.xyz) / vol_bbox_size;

            float4 color_count = voxColorAndCount.SampleLevel(linearSampler, tex_coords, 1.0);
            float3 color = color_count.rgb;
            float count = color_count.w;
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
            float phase_factor = evalPhaseFuncPdf(vol.phase_func_type, vol.phase_func_params, light_dir, -dir);

            float3 power = color * phase_factor;
            float alpha = exp(-extinction * step);
            float3 sample_color = (albedo * power) * step;
            result += sample_color * result_alpha / max(count, 1);
            //result_alpha *= alpha;

            t += step / z_factor * nextRand(randSeed);
        }
    }

    return float4(result, saturate(1.f - length(result * 3)));
}

void main(
    in float4 Pos : SV_Position,
    in float2 Tex : TexCoord0,
    out float4 Color : SV_TARGET0
)
{
    uint randSeed = initRand(Pos.x + Pos.x * Pos.y, perFrameConstants.cameraParams.frameCount);

    float3 normal = gbufferNormals.Load(int3(Pos.xy, 0)).xyz;

    float2 d = Tex.xy * 2.f - 1.f;
    float3 viewDir = normalize(d.x * perFrameConstants.cameraParams.U + (-d.y) * perFrameConstants.cameraParams.V + perFrameConstants.cameraParams.W).xyz;

    float3 lightDir;
    lightDir.x = photonSplatColorXYZDirX.Load(int3(Pos.xy, 0)).w;
    lightDir.yz = photonSplatDirYZ.Load(int3(Pos.xy, 0)).xy;
    lightDir = normalize(lightDir);

    float lightFactor = saturate(dot(normal, lightDir)) / M_PI; //evaluateBrdf(lightDir, viewDir, normal);

    float volumeDepth = gbufferVolumeDepth.Load(int3(Pos.xy, 0));
    bool shouldRaymarch = perFrameConstants.options.volumeSplattingMethod == SplatMethod::Voxels && volumeDepth > 0;
    float4 volumeColor = shouldRaymarch * raymarch(perFrameConstants.cameraParams.worldEyePos.xyz, viewDir, Tex, volumeDepth, randSeed);
    float3 surfaceColor = photonSplatColorXYZDirX.Sample(lightSampler, Tex).xyz * 10;// * lerp(lightFactor, 1.0, perFrameConstants.options.showRawSplattingResult);
    float3 totalColor = volumeColor.rgb + lerp(1.f, volumeColor.a, shouldRaymarch) * surfaceColor;

    Color = float4(totalColor, 1);
}
