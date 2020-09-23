#define HLSL
#include "RasterCommon.hlsli"

ConstantBuffer<PerObjectConstants> obj : register(b1);

struct Input
{
    float3 position : SV_POSITION;
    float3 normal : NORMAL;
};

struct VertexShaderOutput
{
    float4 position : SV_POSITION;
    float3 positionVS : POSITION_VS;
    float3 positionObj : POSITION_OBJ;
    float3 normal : NORMAL;
};

VertexShaderOutput main(Input IN)
{
    VertexShaderOutput OUT;

    float4x4 mvp = mul(perFrameConstants.WorldToViewClipMatrix, (float4x4) obj.worldMatrix);
    float4x4 mv = mul(perFrameConstants.WorldToViewMatrix, (float4x4) obj.worldMatrix);
    OUT.position = mul(mvp, float4(IN.position, 1.0f));
    OUT.positionVS = mul(mv, float4(IN.position, 1.0f));
    OUT.positionObj = IN.position;
    OUT.normal = normalize(mul(obj.worldMatrix, float4(IN.normal, 0.0f)).xyz);

    return OUT;
}
