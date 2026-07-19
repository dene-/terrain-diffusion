cbuffer ReticleUniform : register(b0, space3)
{
    float4 ScreenSizeAndPadding;
};

float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    float2 pixel = (uv - 0.5f) * ScreenSizeAndPadding.xy;
    float horizontal = step(abs(pixel.y), 1.15f) * step(abs(pixel.x), 8.0f) * step(2.5f, abs(pixel.x));
    float vertical = step(abs(pixel.x), 1.15f) * step(abs(pixel.y), 8.0f) * step(2.5f, abs(pixel.y));
    float alpha = saturate(horizontal + vertical) * 0.82f;
    clip(alpha - 0.01f);
    return float4(0.95f, 0.97f, 0.98f, alpha);
}

