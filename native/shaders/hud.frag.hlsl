Texture2D<float4> HudTexture : register(t0, space2);
SamplerState HudSampler : register(s0, space2);

struct Input
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

float4 main(Input input) : SV_Target0
{
    return HudTexture.Sample(HudSampler, input.UV);
}
