#define HLSL
#include "RasterHlslCompat.h"

ConstantBuffer<PerFrameConstantsRaster> perFrameConstants : register(b0);
ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);

Texture2D<float> DepthTexture : register(t0, space1);

struct VSOutput
{
    float4 position : SV_Position;
    float3 power : COLOR;
    float3 direction : DIRECTION_WS;
};

struct PSOutput
{
    float4 ColorXYZAndDirectionX : SV_Target0;
    float2 DirectionYZ : SV_Target1;
};

#define KERNEL_COMPRESS_FACTOR 0.8f

float LinearDepth(float depth)
{
    float zNear = perFrameConstants.cameraParams.frustumNearFar.x;
    float zFar = perFrameConstants.cameraParams.frustumNearFar.y;

/*
    posView = projMatrixInv * (0,0,zClip,1)
    -> zView = -1, wView = ( f + n - zClip(f-n) ) / 2fn
*/
    float zClip = depth * 2.f - 1.f;
    float linDepth = (2.0f * zFar * zNear) / (zFar + zNear - zClip * (zFar - zNear));
    return linDepth; // should be -linDepth but we only use the abs val
}

[earlydepthstencil]
void main(VSOutput IN, out PSOutput OUT)
{
    float gbuffer_linear_depth = LinearDepth(DepthTexture[IN.position.xy]);
    float kernel_linear_depth = LinearDepth(IN.position.z);
    float d = abs(gbuffer_linear_depth - kernel_linear_depth);

    clip(KERNEL_COMPRESS_FACTOR * 1e-3 - d);

    float3 power = IN.power;
    power /= max(power.x, max(power.y, power.z));
    float total_power = dot(power.xyz, float3(1.0f, 1.0f, 1.0f));
    float3 weighted_direction = total_power * IN.direction;

    OUT.ColorXYZAndDirectionX = float4(power, weighted_direction.x);
    OUT.DirectionYZ = weighted_direction.yz;
}
