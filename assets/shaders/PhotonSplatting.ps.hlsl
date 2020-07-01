struct GSOutput
{
	float4 pos : SV_POSITION;
};

[earlydepthstencil]
void PS(
vs_to_ps Input,
out float4 OutputColorXYZAndDirectionX : SV_Target,
out float2 OutputDirectionYZ : SV_Target1)
{
    float depth = DepthTexture[Input.Position.xy];
    float gbuffer_linear_depth = LinearDepth(ViewConstants, depth);
    float kernel_linear_depth = LinearDepth(ViewConstants,
        Input.Position.z);
    float d = abs(gbuffer_linear_depth - kernel_linear_depth);

    clip(d > (KernelCompress * MAX_DEPTH) ? -1 : 1);

    float3 power = Input.Power;
    float total_power = dot(power.xyz, float3(1.0f, 1.0f, 1.0f));
    float3 weighted_direction = total_power * Input.Direction;

    OutputColorXYZAndDirectionX = float4(power, weighted_direction.x);
    OutputDirectionYZ = weighted_direction.yz;
}
