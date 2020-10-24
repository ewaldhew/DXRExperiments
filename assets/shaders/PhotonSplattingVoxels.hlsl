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

static const float INV_SQRT2PI = 1.f / sqrt(2 * M_PI);

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

[numthreads(4, 4, 4)]
void main( uint3 tid : SV_DispatchThreadID, uint offset : SV_GroupIndex )
{
    const uint3 tex_idx = tid.xyz;

    uint3 tex_size;
    voxColorAndCount.GetDimensions(tex_size.x, tex_size.y, tex_size.z);
    const float3 vol_bbox_size = photonMapConsts.volumeBboxMax.xyz - photonMapConsts.volumeBboxMin.xyz;

    const float3 cell_size = vol_bbox_size / tex_size;
    const float3 cell_center_idx = float3(tex_idx) + 0.5f;
    const float3 cell_pos = photonMapConsts.volumeBboxMin.xyz + cell_center_idx * cell_size;

    float3 vox_color = 0.f;
    float factor = 0.f;
    float3 vox_direction = 0.f;
    float mat_idx = 0;

    const int start = photonMapConsts.counts[PhotonMapID::Volume - 1].x;
    const int end = photonMapConsts.counts[PhotonMapID::Volume].x;
    int photon_idx;
    for (photon_idx = start + offset; photon_idx < end; photon_idx += 64) {
        unpacked_photon photon = get_photon(photon_idx);

        float3 color = photon.power;
        float3 direction = photon.direction;

        float3 d = photon.position - cell_pos;
        float drbf2 = dot(d, d);
        float rbf = saturate(INV_SQRT2PI * exp(-0.5f * drbf2));

        vox_color += color * rbf * 100 * 64;
        //factor += rbf;
        vox_direction += direction * rbf;
        mat_idx = max(mat_idx, photon.materialIndex);
    }

    voxColorAndCount[tex_idx] = float4(vox_color, factor);
    voxDirectionAndMatId[tex_idx] = float4(vox_direction, mat_idx);
}
