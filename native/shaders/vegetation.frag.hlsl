Texture2D<float> ShadowMap : register(t0, space2);
SamplerState ShadowSampler : register(s0, space2);
Texture2D<float4> TreeAtlas : register(t1, space2);
SamplerState TreeSampler : register(s1, space2);

cbuffer VegetationFragmentUniform : register(b0, space3)
{
    float4 SunDirectionAndIntensity;
    float4 CameraHeightFogTimeTexel;
    float4 HorizonColor;
    float4 ZenithColor;
    float4 AtmosphereParameters;
    float4 CloudShadowParameters;
    float4 SkyLightingParameters;
};

struct Input
{
    float4 Position : SV_Position;
    float3 Normal : TEXCOORD0;
    float4 Color : TEXCOORD1;
    float4 ShadowPosition : TEXCOORD2;
    float Distance : TEXCOORD3;
    float2 UV : TEXCOORD4;
    float Type : TEXCOORD5;
    float Visibility : TEXCOORD6;
    float SurfaceHeight : TEXCOORD7;
    float3 ViewDirection : TEXCOORD8;
};

float3 authoredSrgbToLinear(float3 color)
{
    float3 low = color / 12.92f;
    float3 high = pow((color + 0.055f) / 1.055f, 2.4f);
    return lerp(low, high, step(0.04045f, color));
}

float3 atmosphericRadiance(float3 viewDirection)
{
    float elevation = saturate(viewDirection.y);
    float3 radiance = lerp(HorizonColor.rgb, ZenithColor.rgb, pow(elevation, 0.56f));
    float3 toSun = normalize(-SunDirectionAndIntensity.xyz);
    float forwardScatter = pow(saturate(dot(viewDirection, toSun)), 24.0f);
    return radiance + float3(1.0f, 0.74f, 0.46f) * forwardScatter * 0.10f;
}

float atmosphericHaze(float distanceToCamera, float cameraHeight, float surfaceHeight)
{
    float baseHeight = AtmosphereParameters.z;
    float cameraDensity = exp(-max(cameraHeight - baseHeight, 0.0f) * AtmosphereParameters.y);
    float surfaceDensity = exp(-max(surfaceHeight - baseHeight, 0.0f) * AtmosphereParameters.y);
    float averageDensity = sqrt(max(cameraDensity * surfaceDensity, 0.0f));
    float opticalDepth = distanceToCamera * AtmosphereParameters.x * averageDensity;
    return saturate((1.0f - exp(-opticalDepth)) * AtmosphereParameters.w);
}

float hash21(float2 position)
{
    position = frac(position * float2(123.34f, 456.21f));
    position += dot(position, position + 45.32f);
    return frac(position.x * position.y);
}

// Rotated poisson taps replace the 2x2 box: per-pixel rotation trades blocky
// texel stair-steps along terminators for stable fine grain.
static const float2 ShadowPoisson[8] = {
    float2(-0.7071f, 0.7071f), float2(-0.0000f, -0.8750f),
    float2(0.5303f, 0.5303f), float2(-0.6250f, -0.0000f),
    float2(0.3536f, -0.3536f), float2(-0.0000f, 0.3750f),
    float2(-0.1768f, -0.1768f), float2(0.1250f, 0.0000f)
};

float shadowVisibility(float4 shadowPosition, float normalLight, float2 screenPosition)
{
    float3 projected = shadowPosition.xyz / max(shadowPosition.w, 0.0001f);
    float2 uv = float2(projected.x * 0.5f + 0.5f, 0.5f - projected.y * 0.5f);
    if (projected.z <= 0.0f || projected.z >= 1.0f || any(uv <= 0.001f) || any(uv >= 0.999f)) return 1.0f;
    float visibility = 0.0f;
    float bias = lerp(0.0014f, 0.00028f, normalLight);
    float angle = hash21(floor(screenPosition)) * 6.2831853f;
    float cosine = cos(angle);
    float sine = sin(angle);
    float2x2 rotation = float2x2(cosine, -sine, sine, cosine);
    [unroll] for (int tap = 0; tap < 8; ++tap)
    {
        float2 offset = mul(rotation, ShadowPoisson[tap]) * (1.4f * CameraHeightFogTimeTexel.w);
        float depth = ShadowMap.SampleLevel(ShadowSampler, uv + offset, 0);
        visibility += projected.z - bias <= depth ? 1.0f : 0.0f;
    }
    return visibility * 0.125f;
}

float4 main(Input input) : SV_Target0
{
    int debugView = (int)round(CameraHeightFogTimeTexel.y);
    if (debugView > 0 && debugView != 12 && debugView != 13) clip(-1.0f);
    clip(input.Visibility - 0.5f);
    const bool grass = input.Type > 0.5f && input.Type < 1.5f;
    float3 albedo = authoredSrgbToLinear(input.Color.rgb);
    if (!grass)
    {
        float4 tree = TreeAtlas.Sample(TreeSampler, input.UV);
        clip(tree.a - (input.Type > 1.5f ? 0.34f : 0.40f));
        albedo *= min(float3(1.0f, 1.0f, 1.0f), pow(max(tree.rgb, 0.001f), 0.68f) * 1.16f);
    }
    if (debugView == 12)
    {
        if (grass) return float4(0.16f, 0.88f, 0.26f, 1.0f);
        return input.Type > 1.5f
            ? float4(0.98f, 0.58f, 0.10f, 1.0f)
            : float4(0.14f, 0.72f, 1.0f, 1.0f);
    }
    if (debugView == 13)
        return float4(input.Distance < 1700.0f
            ? float3(0.12f, 0.82f, 0.25f)
            : float3(0.95f, 0.72f, 0.12f), 1.0f);
    float3 lightDirection = normalize(-SunDirectionAndIntensity.xyz);
    float normalLight = grass
        ? saturate(abs(dot(normalize(input.Normal), lightDirection)) * 0.72f + 0.18f)
        : saturate(abs(dot(normalize(input.Normal), lightDirection)));
    float shadow = grass ? 1.0f : shadowVisibility(input.ShadowPosition, normalLight, input.Position.xy);
    float ambient = (grass ? 0.30f : 0.42f) + SkyLightingParameters.x * 0.24f;
    float lighting = ambient + normalLight * 0.62f * SunDirectionAndIntensity.w * lerp(0.16f, 1.0f, shadow);
    float3 lit = albedo * lighting;
    float haze = atmosphericHaze(input.Distance, max(CameraHeightFogTimeTexel.x, 1.68f), input.SurfaceHeight);
    float valleyDepth = saturate((CameraHeightFogTimeTexel.x - input.SurfaceHeight) / 2600.0f);
    haze = saturate(haze + valleyDepth * (1.0f - exp(-input.Distance * 0.000045f)) * 0.14f);
    return float4(lerp(lit, atmosphericRadiance(normalize(input.ViewDirection)), haze)
        * SkyLightingParameters.y, 1.0f);
}
