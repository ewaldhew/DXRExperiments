#define HLSL
#include "RasterHlslCompat.h"
#include "RasterCommon.hlsli"

ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);

Texture2D<float> DepthTexture : register(t0, space1);

struct VSOutput
{
    uint photonID : PHOTON_ID;
    float4 position : SV_Position;
    float4 positionVS : Position_VS;
    float3 power : COLOR;
    float3 direction : DIRECTION_WS;
};

struct PSOutput
{
    float4 ColorXYZAndDirectionX : SV_Target0;
    float2 DirectionYZ : SV_Target1;
};

[earlydepthstencil]
void main(VSOutput IN, out PSOutput OUT)
{
    float gbuffer_linear_depth = DepthTexture[IN.position.xy];
    float kernel_linear_depth = -IN.positionVS.z;
    float d = abs(gbuffer_linear_depth - kernel_linear_depth);

    if (IN.photonID < photonMapConsts.counts[PhotonMapID::Volume - 1].x) {
        if(perFrameConstants.options.showVolumePhotonsOnly) discard;
        clip(photonMapConsts.kernelCompressFactor * 1e-3 - d);
    } else {
        clip(gbuffer_linear_depth - kernel_linear_depth);
    }

    float3 power = IN.power;
    float total_power = dot(power.xyz, float3(1.0f, 1.0f, 1.0f));
    float3 weighted_direction = total_power * IN.direction;

    OUT.ColorXYZAndDirectionX = float4(power, weighted_direction.x);
    OUT.DirectionYZ = weighted_direction.yz;
}
