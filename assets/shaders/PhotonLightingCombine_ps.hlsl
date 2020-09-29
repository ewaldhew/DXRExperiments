#include "RasterCommon.hlsli"

Texture2D<float4> gbufferNormals : register(t0, space1);

Texture2D<float4> photonSplatColorXYZDirX : register(t1, space1);
Texture2D<float2> photonSplatDirYZ : register(t2, space1);

SamplerState lightSampler : register(s0, space1);

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

    float3 totalColor = photonSplatColorXYZDirX.Sample(lightSampler, Tex).xyz;

    Color = float4(totalColor, 1); //* lerp(lightFactor, 1.0, perFrameConstants.options.showRawSplattingResult);
}
