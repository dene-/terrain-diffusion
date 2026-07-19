Texture2D<float4> TreeAtlas : register(t0, space2);
SamplerState TreeSampler : register(s0, space2);

struct Input
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

float hash21(float2 position)
{
    position = frac(position * float2(123.34f, 345.45f));
    position += dot(position, position + 34.345f);
    return frac(position.x * position.y);
}

void main(Input input)
{
    float alpha = TreeAtlas.Sample(TreeSampler, input.UV).a;
    // Foliage transmits a significant fraction of daylight. Writing every
    // opaque atlas texel into the depth map turns an entire billboard crown
    // into one black ground blob. A stable atlas-space coverage dither gives
    // PCF filtering real canopy gaps without transparency sorting or another
    // shadow target.
    float coverage = saturate((alpha - 0.16f) / 0.84f) * 0.42f;
    clip(coverage - hash21(floor(input.UV * 2048.0f)));
}
