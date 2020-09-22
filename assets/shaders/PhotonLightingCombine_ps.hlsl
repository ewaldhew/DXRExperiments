#include "RasterCommon.hlsli"
#include "ParticipatingMediaUtil.hlsli"

Texture2D<float4> gbufferNormals : register(t0, space1);

Texture2D<float4> photonSplatColorXYZDirX : register(t1, space1);
Texture2D<float2> photonSplatDirYZ : register(t2, space1);

ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);
Texture2D<float> gbufferDepth : register(t0, space2);
Texture2D<float> gbufferVolumeMask : register(t1, space2);
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

float3 raymarch(float3 origin, float3 dir, float2 screen_pos)
{
    float z_factor = dot(dir, normalize(perFrameConstants.cameraParams.W.xyz));
    float gbuffer_linear_depth = -LinearDepth(gbufferDepth[screen_pos]);

    uint3 tex_size;
    voxColorAndCount.GetDimensions(tex_size.x, tex_size.y, tex_size.z);
    float3 vol_bbox_size = photonMapConsts.volumeBboxMax.xyz - photonMapConsts.volumeBboxMin.xyz;
    float3 cell_size = vol_bbox_size / tex_size;
    float step = min(cell_size.x, min(cell_size.y, cell_size.z));

    float3 result;
    float result_alpha;
    [loop]
    for (float t = 0.f; t * z_factor < gbuffer_linear_depth; t += step) {
        float3 curr_pos = origin + dir * t;
        float3 tex_coords = (curr_pos - photonMapConsts.volumeBboxMin.xyz) / vol_bbox_size;

        float4 color_count = voxColorAndCount.SampleLevel(lightSampler, tex_coords, 0.0);
        float3 color = color_count.rgb;
        uint count = uint(round(color_count.w));
        float4 light_dir_mat_id = voxDirectionAndMatId.SampleLevel(linearSampler, tex_coords, 0.0);
        float3 light_dir = light_dir_mat_id.xyz;
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
        float alpha = exp(-extinction * step);
        float3 sample_color = (absorption * emission + albedo * power) * step;
        result += sample_color * result_alpha / max(count, 1);
        result_alpha *= alpha;
    }

    return result * result_alpha;
}

void main(
    in float4 Pos : SV_Position,
    in float2 Tex : TexCoord0,
    out float4 Color : SV_TARGET0
)
{
    float3 normal = gbufferNormals.Load(int3(Pos.xy, 0)).xyz;

    float2 d = Tex.xy * 2.f - 1.f;
    float3 viewDir = normalize(d.x * perFrameConstants.cameraParams.U + (-d.y) * perFrameConstants.cameraParams.V + perFrameConstants.cameraParams.W).xyz;

    float3 lightDir;
    lightDir.x = photonSplatColorXYZDirX.Load(int3(Pos.xy, 0)).w;
    lightDir.yz = photonSplatDirYZ.Load(int3(Pos.xy, 0)).xy;
    lightDir = normalize(lightDir);

    float lightFactor = saturate(dot(normal, lightDir)) / M_PI; //evaluateBrdf(lightDir, viewDir, normal);

    bool shouldRaymarch = perFrameConstants.options.volumeSplattingMethod == SplatMethod::Voxels;
    float3 volumeColor = shouldRaymarch * gbufferVolumeMask.Load(int3(Pos.xy, 0)) * raymarch(perFrameConstants.cameraParams.worldEyePos.xyz, viewDir, Tex);
    float3 surfaceColor = photonSplatColorXYZDirX.Sample(lightSampler, Tex).xyz;
    float3 totalColor = volumeColor + surfaceColor;

    Color = float4(totalColor, 1); //* lerp(lightFactor, 1.0, perFrameConstants.options.showRawSplattingResult);
}
