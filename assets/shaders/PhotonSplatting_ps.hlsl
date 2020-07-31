struct VSOutput
{
    float4 position : SV_Position;
    float3 power : COLOR;
    float3 direction : DIRECTION_WS;
};

struct PSOutput
{
    float4 OutputColorXYZAndDirectionX : SV_Target0;
    float2 OutputDirectionYZ : SV_Target1;
};

[earlydepthstencil]
void main(VSOutput IN, out PSOutput OUT)
{
    //float depth = DepthTexture[Input.position.xy];
    //float gbuffer_linear_depth = LinearDepth(ViewConstants, depth);
    //float kernel_linear_depth = LinearDepth(ViewConstants, Input.position.z);
    //float d = abs(gbuffer_linear_depth - kernel_linear_depth);

    //clip(d > (KernelCompress * MAX_DEPTH) ? -1 : 1);

    //float3 power = Input.Power;
    //float total_power = dot(power.xyz, float3(1.0f, 1.0f, 1.0f));
    //float3 weighted_direction = total_power * Input.Direction;

    //OutputColorXYZAndDirectionX = float4(power, weighted_direction.x);
    //OutputDirectionYZ = weighted_direction.yz;
}
