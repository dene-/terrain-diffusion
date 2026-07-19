Texture2D<float4> CloudTexture : register(t0, space2);
SamplerState CloudSampler : register(s0, space2);

float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    uint width;
    uint height;
    CloudTexture.GetDimensions(width, height);
    float2 texel = 1.0f / float2(max(width, 1u), max(height, 1u));
    float4 center = CloudTexture.Sample(CloudSampler, uv);
    float4 neighbourhood = (
        CloudTexture.Sample(CloudSampler, uv + float2(texel.x, 0.0f))
        + CloudTexture.Sample(CloudSampler, uv - float2(texel.x, 0.0f))
        + CloudTexture.Sample(CloudSampler, uv + float2(0.0f, texel.y))
        + CloudTexture.Sample(CloudSampler, uv - float2(0.0f, texel.y))) * 0.25f;
    // Recover a little edge definition lost by the quarter-resolution volume
    // without increasing the expensive ray-march pixel count.
    float4 sharpened = center + (center - neighbourhood) * 0.28f;
    sharpened.rgb = max(sharpened.rgb, 0.0f);
    sharpened.a = saturate(sharpened.a);
    return sharpened;
}
