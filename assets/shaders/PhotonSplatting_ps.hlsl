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

[earlydepthstencil]
void main(VSOutput IN, out PSOutput OUT)
{
    //float depth = DepthTexture[Input.position.xy];
    //float gbuffer_linear_depth = LinearDepth(ViewConstants, depth);
    //float kernel_linear_depth = LinearDepth(ViewConstants, Input.position.z);
    //float d = abs(gbuffer_linear_depth - kernel_linear_depth);

    //clip(d > (KernelCompress * MAX_DEPTH) ? -1 : 1);

    float3 power = IN.power;
    power /= max(power.x, max(power.y, power.z));
    float total_power = dot(power.xyz, float3(1.0f, 1.0f, 1.0f));
    float3 weighted_direction = total_power * IN.direction;

    OUT.ColorXYZAndDirectionX = float4(power, weighted_direction.x);
    OUT.DirectionYZ = weighted_direction.yz;
}
