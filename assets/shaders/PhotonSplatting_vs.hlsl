#define HLSL
#include "RasterHlslCompat.h"
#include "RaytracingUtils.hlsli"

StructuredBuffer<Photon> photonBuffer : register(t0);
Texture2D<uint> photonDensity : register(t1);

ConstantBuffer<PerFrameConstantsRaster> perFrameConstants : register(b0);
ConstantBuffer<PhotonMappingConstants> photonMapConsts : register(b1);

struct VSInput
{
    float3 Position : SV_Position;
    uint instanceID : SV_InstanceID;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 power : COLOR;
    float3 direction : DIRECTION_WS;
};

struct unpacked_photon
{
    float3 position;
    float3 power;
    float3 direction;
    float3 normal;
    float distTravelled;
};

struct kernel_output
{
    float3 vertex_position;
    float ellipse_area;
    float scaling_uniform;
    float light_shaping_scale;
};

#define DYNAMIC_KERNEL_SCALE_MIN .01f
#define DYNAMIC_KERNEL_SCALE_MAX 1.0f
#define MAX_SCALING_CONSTANT 5.f
#define KERNEL_COMPRESS_FACTOR 0.8f

// E. Haines, T. Akenine-Möller (eds.), Ray Tracing Gems, https://doi.org/10.1007/978-1-4842-4427-2_24
float uniform_scaling(float3 pos, float ray_length)
{
    // Tile-based culling as photon density estimation
    float4 posClip = mul(perFrameConstants.WorldToViewClipMatrix, float4(pos, 1));
    float2 posScreen = ((posClip / posClip.w).xy + 1.f) * 0.5f;
    uint2 tile = uint2(posScreen.x * photonMapConsts.numTiles.x, (1.0 - posScreen.y) * photonMapConsts.numTiles.y);
    int n_p = max(1, photonDensity.Load(int3(tile, 0)));
    const float n_tile = 3.2f;

    // Equation 24.5
    float a_view = pos.z * pos.z * photonMapConsts.tileAreaConstant;
    float r = sqrt(a_view / (M_PI * n_p));

    // Equation 24.6
    float s_d = clamp(r, DYNAMIC_KERNEL_SCALE_MIN, DYNAMIC_KERNEL_SCALE_MAX) * n_tile;

    // Equation 24.2
    float s_l = clamp(ray_length / photonMapConsts.maxRayLength, .1f, 1.0f);
    return s_d * s_l;
}
//
kernel_output kernel_modification_for_vertex_position(float3 vertex, float3 n, float3 light, float3 pp, float ray_length)
{
    kernel_output o;
    o.scaling_uniform = uniform_scaling(pp, ray_length);

    float3 l = normalize(light);
    float3 cos_alpha = dot(n, vertex);
    float3 projected_v_to_n = cos_alpha * n;
    float cos_theta = saturate(dot(n, l));
    float3 projected_l_to_n = cos_theta * n;

    float3 u = normalize(l - projected_l_to_n);

    // Equation 24.7
    o.light_shaping_scale = min(1.0f/cos_theta, MAX_SCALING_CONSTANT);

    float3 projected_v_to_u = dot(u, vertex) * u;
    float3 projected_v_to_t = vertex - projected_v_to_u;
    projected_v_to_t -= dot(projected_v_to_t, n) * n;

    // Equation 24.8
    float3 scaled_u = projected_v_to_u * o.light_shaping_scale * o.scaling_uniform;
    float3 scaled_t = projected_v_to_t * o.scaling_uniform;
    float3 scaled_n = projected_v_to_n * KERNEL_COMPRESS_FACTOR * o.scaling_uniform;
    o.vertex_position = scaled_u + scaled_t + scaled_n;

    float min_scale = DYNAMIC_KERNEL_SCALE_MIN * .1f;
    float min_area = min_scale * min_scale * 1.0f;
    o.ellipse_area = o.scaling_uniform  * o.scaling_uniform * o.light_shaping_scale;
    o.ellipse_area /= min_area;

    return o;
}


unpacked_photon get_photon(uint index)
{
    Photon photon = photonBuffer[index];

    unpacked_photon result;
    result.position = photon.position;
    result.power = photon.power;
    result.direction = spherical_to_unitvec(photon.direction);
    result.normal = spherical_to_unitvec(photon.normal);
    result.distTravelled = photon.distTravelled;

    return result;
}

void main(in VSInput IN, out VSOutput OUT)
{
    unpacked_photon up = get_photon(IN.instanceID);
    float3 photon_position = up.position;
    kernel_output o = kernel_modification_for_vertex_position(IN.Position, up.normal, -up.direction, up.position, up.distTravelled);

    float3 position = photon_position + o.vertex_position;

    OUT.position = mul(perFrameConstants.WorldToViewClipMatrix, float4(position, 1));
    OUT.power = up.power / o.ellipse_area;
    OUT.direction = -up.direction;
}
