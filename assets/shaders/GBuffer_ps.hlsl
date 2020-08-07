#define HLSL
#include "RasterCommon.hlsli"

cbuffer PerObjectConstants : register(b1)
{
    float4x4 worldMatrix;
}

struct PixelShaderInput
{
    float4 position : SV_POSITION;
    float3 normal : NORMAL;
    float3 positionObjSpace : POSITION_OBJ;
};

struct PixelShaderOutput
{
    float4 gbNormal : SV_TARGET0;
    //float4 gbAlbedo : SV_TARGET1;
};

PixelShaderOutput main(PixelShaderInput IN)
{
    PixelShaderOutput OUT;

    MaterialParams mat = materialParams;
    collectMaterialParams(mat, IN.positionObjSpace);

    float4 albedo = mat.albedo;
    if (albedo.a == 0.0f)
    {
        discard;//discard this pixel
    }

    OUT.gbNormal = float4(normalize(IN.normal), 0.0f);
    //OUT.gbAlbedo = float4(albedo.xyz * albedo.a, 1.0f);

    return OUT;
}
