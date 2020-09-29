#define HLSL
#include "RasterHlslCompat.h"
#include "RaytracingUtils.hlsli"

Texture2D<uint> photonDensity : register(t0);
StructuredBuffer<Photon> volPhotonBuffer : register(t1);
Buffer<float4> volPhotonPosObj : register(t2);

ConstantBuffer<PerFrameConstantsRaster> perFrameConstants : register(b0);
ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);

RWTexture3D<float4> voxColorAndCount : register(u0);
RWTexture3D<float4> voxDirectionAndMatId : register(u1);

struct unpacked_photon
{
    float3 position;
    float3 power;
    float3 direction;
    float3 normal;
    float distTravelled;
    uint materialIndex;
};

unpacked_photon get_photon(uint index)
{
    Photon photon = volPhotonBuffer[index];

    unpacked_photon result;
    result.position = photon.position;
    result.power = photon.power;
    result.direction = spherical_to_unitvec(photon.direction);
    result.normal = spherical_to_unitvec(photon.normal);
    result.distTravelled = photon.distTravelled;
    result.materialIndex = photon.materialIndex;

    return result;
}

[numthreads(64, 1, 1)]
void main( uint3 tid : SV_DispatchThreadID )
{
    uint photon_idx = tid.x;
    unpacked_photon photon = get_photon(photon_idx);

    float3 color = photon.power;
    float3 direction = photon.direction;

    uint3 tex_size;
    voxColorAndCount.GetDimensions(tex_size.x, tex_size.y, tex_size.z);
    float3 vol_bbox_size = photonMapConsts.volumeBboxMax.xyz - photonMapConsts.volumeBboxMin.xyz;
    float3 tex_coords = (photon.position - photonMapConsts.volumeBboxMin.xyz) / vol_bbox_size;
    uint3 tex_idx = uint3(tex_coords * tex_size);
//    float3 cell_size = vol_bbox_size / tex_size;

    voxColorAndCount[tex_idx] += float4(color, 1);
    voxDirectionAndMatId[tex_idx].xyz += direction;
    voxDirectionAndMatId[tex_idx].w = photon.materialIndex;
}
