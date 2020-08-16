Texture2D<float4> gbufferNormals : register(t0);

Texture2D<float4> photonSplatColorXYZDirX : register(t1);
Texture2D<float2> photonSplatDirYZ : register(t2);

void main(
    in float4 Pos : SV_Position,
    in float2 Tex : TexCoord0,
    out float4 Color : SV_TARGET0
)
{
    Color = float4(Tex, 1, 1);
}
