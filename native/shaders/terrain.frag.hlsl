Texture2D<float> ShadowMap : register(t0, space2);
SamplerState ShadowSampler : register(s0, space2);
Texture2D<float> FarShadowMap : register(t1, space2);
SamplerState FarShadowSampler : register(s1, space2);

cbuffer TerrainFragmentUniform : register(b0, space3)
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
    float3 LocalWorld : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Color : TEXCOORD2;
    float4 ShadowPosition : TEXCOORD3;
    float4 DistanceAndRadii : TEXCOORD4;
    float2 NoisePosition : TEXCOORD5;
    float4 FarShadowPosition : TEXCOORD6;
    float4 Derived : TEXCOORD7;
    float4 Biome : TEXCOORD8;
    float LodLevel : TEXCOORD9;
    float2 TileCoordinates : TEXCOORD10;
    float3 ViewDirection : TEXCOORD11;
};

float3 atmosphericRadiance(float3 viewDirection)
{
    float elevation = saturate(viewDirection.y);
    float3 radiance = lerp(HorizonColor.rgb, ZenithColor.rgb, pow(elevation, 0.56f));
    float3 toSun = normalize(-SunDirectionAndIntensity.xyz);
    float forwardScatter = pow(saturate(dot(viewDirection, toSun)), 24.0f);
    return radiance + float3(1.0f, 0.74f, 0.46f) * forwardScatter * 0.10f;
}

float hash21(float2 position)
{
    position = frac(position * float2(123.34f, 456.21f));
    position += dot(position, position + 45.32f);
    return frac(position.x * position.y);
}

float valueNoise(float2 position)
{
    float2 cell = floor(position);
    float2 local = frac(position);
    local = local * local * (3.0f - 2.0f * local);
    return lerp(
        lerp(hash21(cell), hash21(cell + float2(1, 0)), local.x),
        lerp(hash21(cell + float2(0, 1)), hash21(cell + 1), local.x),
        local.y);
}

float3 authoredSrgbToLinear(float3 color)
{
    float3 low = color / 12.92f;
    float3 high = pow((color + 0.055f) / 1.055f, 2.4f);
    return lerp(low, high, step(0.04045f, color));
}

float2 rotatePoint(float2 coordinate, float cosine, float sine)
{
    return float2(coordinate.x * cosine - coordinate.y * sine,
        coordinate.x * sine + coordinate.y * cosine);
}

float triplanarNoise(float3 position, float3 normal, float scale)
{
    float3 weights = pow(abs(normal), 4.0f);
    weights /= max(weights.x + weights.y + weights.z, 0.0001f);
    float xProjection = valueNoise(rotatePoint(position.yz * scale, 0.8192f, 0.5736f));
    float yProjection = valueNoise(rotatePoint(position.xz * scale, 0.9171f, -0.3987f));
    float zProjection = valueNoise(rotatePoint(position.xy * scale, 0.6216f, 0.7833f));
    return dot(float3(xProjection, yProjection, zProjection), weights);
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

// Cloud shadows sample the exact field the cloud volume ray-marches, so the
// darkened patches on the ground correspond to the clouds overhead and drift,
// shear and reshape in sync with them. The hash constants must stay identical
// to cloud_overlay.frag.hlsl.
float cloudHash21(float2 position)
{
    position = frac(position * float2(123.34f, 345.45f));
    position += dot(position, position + 34.345f);
    return frac(position.x * position.y);
}

float cloudValueNoise(float2 position)
{
    float2 cell = floor(position);
    float2 local = frac(position);
    local = local * local * (3.0f - 2.0f * local);
    return lerp(
        lerp(cloudHash21(cell), cloudHash21(cell + float2(1, 0)), local.x),
        lerp(cloudHash21(cell + float2(0, 1)), cloudHash21(cell + 1), local.x),
        local.y);
}

float cloudSunVisibility(float2 worldPosition, float surfaceHeight, float distanceToCamera)
{
    float3 toSun = normalize(-SunDirectionAndIntensity.xyz);
    float travelToCloud = max((SkyLightingParameters.w - surfaceHeight) / max(toSun.y, 0.08f), 0.0f);
    float2 cloudPosition = worldPosition + toSun.xz * travelToCloud;
    float time = CameraHeightFogTimeTexel.z;
    float2 wind = float2(0.84f, 0.54f) * time * CloudShadowParameters.w;
    float2 coordinate = (cloudPosition + wind) * CloudShadowParameters.z;
    float weather = cloudValueNoise(coordinate) * 0.72f
        + cloudValueNoise(coordinate * 2.07f + 37.1f + float2(-0.0009f, 0.0006f) * time) * 0.28f;
    float cells = cloudValueNoise(coordinate * 4.35f + float2(0.0029f, -0.0017f) * time + 5.83f);
    float threshold = 1.0f - CloudShadowParameters.x;
    float scatterGate = smoothstep(threshold - 0.20f, threshold + 0.04f, weather);
    float shape = saturate((weather - threshold) / max(CloudShadowParameters.x * 0.42f, 0.04f));
    shape = saturate(shape * lerp(0.38f, 1.38f, cells) + (cells - 0.70f) * 1.20f * scatterGate);
    // Keep the opacity ramp gentle enough that the cells octave survives as
    // dappled light gaps; a hard saturate turns whole cloud systems into one
    // uniform dark blanket kilometres across.
    float opacity = saturate(shape * CloudShadowParameters.y * 1.35f);
    // Cloud shadows wash out under aerial perspective with distance, which
    // also keeps the far field free of hard shadow edges.
    opacity *= 1.0f - smoothstep(9000.0f, 36000.0f, distanceToCamera);
    return 1.0f - opacity * 0.50f;
}

// Rotated poisson taps replace the small box filter: per-pixel rotation
// trades blocky texel stair-steps along terminators for stable fine grain.
static const float2 ShadowPoisson[12] = {
    float2(-0.3262f, -0.4058f), float2(-0.8401f, -0.0736f),
    float2(-0.6959f, 0.4571f), float2(-0.2033f, 0.6207f),
    float2(0.9623f, -0.1950f), float2(0.4734f, -0.4800f),
    float2(0.5195f, 0.7670f), float2(0.1855f, -0.8931f),
    float2(0.5074f, 0.0644f), float2(0.8964f, 0.4125f),
    float2(-0.3219f, -0.9326f), float2(-0.6543f, -0.7916f)
};

float shadowVisibility(float4 shadowPosition, float normalLight, float2 screenPosition)
{
    float3 projected = shadowPosition.xyz / max(shadowPosition.w, 0.0001f);
    float2 uv = projected.xy * 0.5f + 0.5f;
    if (projected.z <= 0.0f || projected.z >= 1.0f || any(uv <= 0.001f) || any(uv >= 0.999f)) return 1.0f;
    float visibility = 0.0f;
    float bias = lerp(0.0012f, 0.00018f, normalLight);
    float angle = hash21(floor(screenPosition)) * 6.2831853f;
    float cosine = cos(angle);
    float sine = sin(angle);
    float2x2 rotation = float2x2(cosine, -sine, sine, cosine);
    [unroll] for (int tap = 0; tap < 12; ++tap)
    {
        float2 offset = mul(rotation, ShadowPoisson[tap]) * (1.4f * CameraHeightFogTimeTexel.w);
        float depth = ShadowMap.SampleLevel(ShadowSampler, uv + offset, 0);
        visibility += projected.z - bias <= depth ? 1.0f : 0.0f;
    }
    return visibility / 12.0f;
}

float farShadowVisibility(float4 shadowPosition, float normalLight, float2 screenPosition)
{
    float3 projected = shadowPosition.xyz / max(shadowPosition.w, 0.0001f);
    float2 uv = projected.xy * 0.5f + 0.5f;
    if (projected.z <= 0.0f || projected.z >= 1.0f || any(uv <= 0.001f) || any(uv >= 0.999f)) return 1.0f;
    float visibility = 0.0f;
    float bias = lerp(0.0022f, 0.00045f, normalLight);
    float angle = hash21(floor(screenPosition) + 17.0f) * 6.2831853f;
    float cosine = cos(angle);
    float sine = sin(angle);
    float2x2 rotation = float2x2(cosine, -sine, sine, cosine);
    [unroll] for (int tap = 0; tap < 8; ++tap)
    {
        // Far texels span metres, so a wider disk keeps distant terminators
        // smooth; eight taps are plenty once the grain is this fine.
        float2 offset = mul(rotation, ShadowPoisson[tap]) * (2.4f * CameraHeightFogTimeTexel.w);
        float depth = FarShadowMap.SampleLevel(FarShadowSampler, uv + offset, 0);
        visibility += projected.z - bias <= depth ? 1.0f : 0.0f;
    }
    return visibility * 0.125f;
}

float4 main(Input input) : SV_Target0
{
    // The ocean owns all fragments at and below mean sea level. Keeping the
    // submerged terrain surface in the opaque depth buffer made the water
    // intersect every shoreline triangle and produced the reported strips.
    clip(input.LocalWorld.y - 0.04f);
    float distanceToCamera = input.DistanceAndRadii.x;
    float surfaceDistance = input.DistanceAndRadii.y;
    float inner = input.DistanceAndRadii.z;
    float outer = input.DistanceAndRadii.w;
    float outerTransition = max(32.0f, min(outer * 0.08f, 9000.0f));
    float innerTransition = max(24.0f, min(inner * 0.16f, 1800.0f));
    // Parent height fields are intentionally much coarser and can differ by
    // hundreds of metres from a detailed child. Exclude them from the inner
    // child footprint instead of relying on a tiny depth offset, which lets a
    // coarse surface pass above the camera. The broad overlap and matching
    // screen-space dither keep both transitions covered.
    float innerCoverage = inner <= 0.5f
        ? 1.0f
        : smoothstep(inner - innerTransition, inner + innerTransition, surfaceDistance);
    float outerCoverage = 1.0f - smoothstep(outer - outerTransition, outer, surfaceDistance);
    float ring = innerCoverage * outerCoverage;
    // clip(0) survives in HLSL. A raw hash can be exactly zero, which leaked
    // isolated pixels from rings whose coverage was otherwise zero.
    clip(ring - 0.001f);
    float coverageThreshold = 0.001f + hash21(floor(input.Position.xy * 0.5f)) * 0.998f;
    clip(ring - coverageThreshold);

    // Rotating the material bands independently avoids axis-aligned repetition
    // across broad slopes while retaining metre-scale detail at eye level.
    float fine = valueNoise(rotatePoint(input.NoisePosition, 0.8192f, 0.5736f) * 0.72f);
    float medium = valueNoise(rotatePoint(input.NoisePosition, 0.9171f, -0.3987f) * 0.073f);
    float broad = valueNoise(rotatePoint(input.NoisePosition, 0.6216f, 0.7833f) * 0.0052f);
    float macro = valueNoise(rotatePoint(input.NoisePosition, 0.3624f, -0.9320f) * 0.00082f);
    float continental = valueNoise(rotatePoint(input.NoisePosition, 0.9801f, 0.1987f) * 0.000025f);
    float slope = input.Derived.r;
    float curvature = input.Derived.g * 2.0f - 1.0f;
    float wetness = input.Derived.b;
    float macroField = input.Derived.a;
    float forestField = input.Biome.r;
    float rockMask = input.Biome.g;
    float screeMask = input.Biome.b * (1.0f - rockMask);
    float snowMask = input.Biome.a;
    float3 normal = normalize(input.Normal);
    float3 albedo = authoredSrgbToLinear(input.Color.rgb);
    float closeDetail = 1.0f - smoothstep(550.0f, 2400.0f, distanceToCamera);
    float mediumDetail = 1.0f - smoothstep(4000.0f, 28000.0f, distanceToCamera);
    float broadDetail = 1.0f - smoothstep(28000.0f, 180000.0f, distanceToCamera);
    float macroDetail = 1.0f - smoothstep(180000.0f, 650000.0f, distanceToCamera);
    albedo *= 0.82f + fine * 0.10f * closeDetail + medium * 0.13f * mediumDetail
        + broad * 0.08f * broadDetail + macro * 0.10f * macroDetail + continental * 0.14f;
    float rockBreakup = triplanarNoise(input.LocalWorld, normal, 0.055f);
    float rockMacro = triplanarNoise(input.LocalWorld, normal, 0.0042f);
    float3 rockColor = lerp(authoredSrgbToLinear(float3(0.23f, 0.24f, 0.25f)),
        authoredSrgbToLinear(float3(0.42f, 0.40f, 0.37f)),
        rockBreakup * 0.52f + rockMacro * 0.48f);
    float3 screeColor = lerp(authoredSrgbToLinear(float3(0.30f, 0.29f, 0.27f)),
        authoredSrgbToLinear(float3(0.46f, 0.43f, 0.37f)), medium);
    float3 forestColor = albedo * lerp(float3(0.50f, 0.68f, 0.44f), float3(0.38f, 0.58f, 0.34f), wetness);
    float3 snowColor = lerp(authoredSrgbToLinear(float3(0.69f, 0.75f, 0.78f)),
        authoredSrgbToLinear(float3(0.93f, 0.95f, 0.96f)), broad);
    albedo = lerp(albedo, screeColor, screeMask);
    albedo = lerp(albedo, rockColor, rockMask * (1.0f - snowMask));
    albedo = lerp(albedo, snowColor, snowMask);
    albedo *= lerp(0.90f, 1.08f, macroField);
    // Beyond individual-tree range, a continuous canopy response keeps
    // forests readable from aircraft height without drawing millions of
    // overlapping billboards. It is world-continuous, never tile-local.
    float farCanopy = forestField * (1.0f - smoothstep(0.16f, 0.52f, slope)) * smoothstep(1500.0f, 2600.0f, distanceToCamera);
    albedo = lerp(albedo, forestColor, farCanopy * 0.72f);

    float3 lightDirection = normalize(-SunDirectionAndIntensity.xyz);
    float normalLight = saturate(dot(normal, lightDirection));
    float closeShadow = shadowVisibility(input.ShadowPosition, normalLight, input.Position.xy);
    float farShadow = farShadowVisibility(input.FarShadowPosition, normalLight, input.Position.xy);
    float shadow = lerp(closeShadow, farShadow, smoothstep(1700.0f, 2350.0f, distanceToCamera));
    shadow *= cloudSunVisibility(input.NoisePosition, input.LocalWorld.y, distanceToCamera);
    float formOcclusion = lerp(0.82f, 1.0f, saturate(normal.y * 0.72f + curvature * 0.20f + 0.28f));
    // Calibrated for the linear HDR target: the previous ambient + direct
    // terms exceeded one even on ordinary grass, lifting authored mid-tones
    // into the ACES shoulder and making daylight look bleached.
    float diffuse = (0.14f + SkyLightingParameters.x * 0.18f
        + normalLight * SunDirectionAndIntensity.w * 0.72f * lerp(0.14f, 1.0f, shadow)) * formOcclusion;
    float3 lit = albedo * diffuse;

    float cameraHeight = max(CameraHeightFogTimeTexel.x, 1.68f);
    float haze = atmosphericHaze(distanceToCamera, cameraHeight, input.LocalWorld.y);
    float valleyDepth = saturate((cameraHeight - input.LocalWorld.y) / 2600.0f);
    haze = saturate(haze + valleyDepth * (1.0f - exp(-distanceToCamera * 0.000045f)) * 0.14f);
    // Atmosphere is continuous in world/view space. Never fade by a tile
    // ring's outer radius: every LOD has a different radius, so doing so
    // produces the cyan circular bands that become obvious from aircraft.
    float3 atmosphere = atmosphericRadiance(normalize(input.ViewDirection));
    float3 finalColor = lerp(lit, atmosphere, haze) * SkyLightingParameters.y;

    int debugView = (int)round(CameraHeightFogTimeTexel.y);
    if (debugView == 1)
    {
        static const float3 lodColors[8] = {
            float3(0.15f, 0.85f, 0.25f), float3(0.95f, 0.75f, 0.12f),
            float3(0.95f, 0.28f, 0.12f), float3(0.74f, 0.22f, 0.90f),
            float3(0.14f, 0.62f, 0.95f), float3(0.95f, 0.30f, 0.62f),
            float3(0.30f, 0.94f, 0.84f), float3(0.88f, 0.88f, 0.88f)
        };
        finalColor = lodColors[min((int)round(input.LodLevel), 7)];
        float edgeDistance = min(min(input.TileCoordinates.x, 1.0f - input.TileCoordinates.x),
            min(input.TileCoordinates.y, 1.0f - input.TileCoordinates.y));
        finalColor = lerp(float3(1.0f, 1.0f, 1.0f), finalColor, smoothstep(0.0f, 0.018f, edgeDistance));
    }
    else if (debugView == 2)
    {
        float elevation = saturate((input.LocalWorld.y + 1000.0f) / 9000.0f);
        finalColor = elevation.xxx;
    }
    else if (debugView == 3) finalColor = slope.xxx;
    else if (debugView == 4) finalColor = float3(saturate(-curvature * 3.0f), 1.0f - abs(curvature) * 3.0f, saturate(curvature * 3.0f));
    else if (debugView == 5) finalColor = float3(0.08f, wetness * 0.72f, wetness);
    else if (debugView == 6) finalColor = float3(forestField * 0.12f, forestField, forestField * 0.18f);
    else if (debugView == 7) finalColor = float3(rockMask, rockMask * 0.46f, rockMask * 0.16f);
    else if (debugView == 8) finalColor = float3(screeMask, screeMask * 0.76f, screeMask * 0.32f);
    else if (debugView == 9) finalColor = snowMask.xxx;
    else if (debugView == 10) finalColor = normal * 0.5f + 0.5f;
    else if (debugView == 11) finalColor = float3(haze, haze * 0.68f, 1.0f - haze);
    else if (debugView == 12) finalColor = lerp(float3(0.10f, 0.12f, 0.14f), float3(0.34f, 0.38f, 0.40f), normal.y);
    else if (debugView == 13)
    {
        // Near-cascade diagnostic: R = receiver depth, G = sampled map depth,
        // B = inside the valid shadow projection. This bypasses all material
        // and cloud shading, distinguishing an empty map from bad UVs or a
        // failed depth comparison.
        float3 projected = input.ShadowPosition.xyz / max(input.ShadowPosition.w, 0.0001f);
        float2 uv = projected.xy * 0.5f + 0.5f;
        float inside = projected.z > 0.0f && projected.z < 1.0f
            && all(uv > 0.001f) && all(uv < 0.999f) ? 1.0f : 0.0f;
        float mapDepth = ShadowMap.SampleLevel(ShadowSampler, saturate(uv), 0);
        finalColor = float3(saturate(projected.z), mapDepth, inside);
    }
    return float4(finalColor, 1.0f);
}
