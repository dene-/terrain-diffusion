Texture2D<float4> SceneColor : register(t0, space2);
SamplerState SceneSampler : register(s0, space2);

float3 acesFilm(float3 value)
{
    const float a = 2.51f;
    const float b = 0.03f;
    const float c = 2.43f;
    const float d = 0.59f;
    const float e = 0.14f;
    return saturate((value * (a * value + b)) / (value * (c * value + d) + e));
}

float3 linearToSrgb(float3 value)
{
    float3 low = value * 12.92f;
    float3 high = 1.055f * pow(max(value, 0.0f), 1.0f / 2.4f) - 0.055f;
    return lerp(low, high, step(0.0031308f, value));
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    float3 sceneLinear = max(SceneColor.Sample(SceneSampler, uv).rgb, 0.0f);
    return float4(linearToSrgb(acesFilm(sceneLinear)), 1.0f);
}
