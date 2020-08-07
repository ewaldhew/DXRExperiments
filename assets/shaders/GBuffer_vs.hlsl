#define HLSL
#include "RasterCommon.hlsli"

cbuffer PerObjectConstants : register(b1)
{
    float4x4 worldMatrix;
}

struct Input
{
    float3 position : SV_POSITION;
    float3 normal : NORMAL;
};

struct VertexShaderOutput
{
    float4 position : SV_POSITION;
    float3 positionObj : POSITION_OBJ;
    float3 normal : NORMAL;
};

VertexShaderOutput main(Input IN)
{
    VertexShaderOutput OUT;

    float4x4 mvp = mul(perFrameConstants.WorldToViewClipMatrix,  worldMatrix);
    OUT.position = mul(mvp, float4(IN.position, 1.0f));
    OUT.positionObj = IN.position;
    OUT.normal = mul(worldMatrix, float4(IN.normal, 0.0f)).xyz;

    return OUT;
}
