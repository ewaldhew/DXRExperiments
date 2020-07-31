struct VSInput
{
    float3 Position : POSITION;
    uint instanceID : SV_InstanceID;
};

struct VSOutput
{
    float4 position : SV_Position;
    float3 power : COLOR;
    float3 direction : DIRECTION_WS;
};

/*
float uniform_scaling(float3 pp_in_view, float ray_length)
{
    // Tile-based culling as photon density estimation
    int n_p = load_number_of_photons_in_tile(pp_in_view);
    float r = .1f;

    if (layers > .0f)
    {
        // Equation 24.5
        float a_view = pp_in_view.z * pp_in_view.z * TileAreaConstant;
        r = sqrt(a_view / (PI * n_p));
    }
    // Equation 24.6
    float s_d = clamp(r, DYNAMIC_KERNEL_SCALE_MIN,
        DYNAMIC_KERNEL_SCALE_MAX) * n_tile;

    // Equation 24.2
    float s_l = clamp(ray_length / MAX_RAY_LENGTH, .1f, 1.0f);
    return s_d * s_l;
}

kernel_output kernel_modification_for_vertex_position(float3 vertex,
    float3 n, float3 light, float3 pp_in_view, float ray_length)
{
    kernel_output o;
    float scaling_uniform =  uniform_scaling(pp_in_view,  ray_length);

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
    float3 scaled_u = projected_v_to_u * light_shaping_scale *
        scaling_Uniform;
    float3 scaled_t = projected_v_to_t * scaling_uniform;
    o.vertex_position = scaled_u + scaled_t +
        (KernelCompress * projected_v_to_n);

    o.ellipse_area = PI * o.scaling_uniform  * o.scaling_uniform *
        o.light_shaping_scale;

    return o;
}
*/
void main(in VSInput IN, out VSOutput OUT)
{
    OUT.position = 0.;
    //unpacked_photon up = unpack_photon(PhotonBuffer[instanceID]);
    //float3 photon_position = up.position;
    //float3 photon_position_in_view = mul(WorldToViewMatrix,
    //float4(photon_position, 1)).xyz;
    //kernel_output o = kernel_modification_for_vertex_position(Position,
    //up.normal, -up.direction, photon_position_in_view, up.ray_length);

    //float3 p = pp + o.vertex_position;

    //Output.position = mul(WorldToViewClipMatrix, float4(p, 1));
    //Output.power = up.power / o.ellipse_area;
    //Output.direction = -up.direction;
}
