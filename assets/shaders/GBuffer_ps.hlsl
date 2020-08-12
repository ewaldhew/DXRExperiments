#define HLSL
#include "RasterCommon.hlsli"

ConstantBuffer<PrimitiveInstanceConstants> aabbCB : register(b1, space1);
ConstantBuffer<PerObjectConstants> obj : register(b1);

struct PixelShaderInput
{
    float4 position : SV_POSITION;
    float3 positionObjSpace : POSITION_OBJ;
    float3 normal : NORMAL;
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
