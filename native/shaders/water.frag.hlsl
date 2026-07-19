cbuffer WaterFragmentUniform : register(b0, space3)
{
    float4 CameraHeightFogTime;
    float4 HorizonColor;
    float4 ZenithColor;
    float4 AtmosphereParameters;
    float4 SkyLightingParameters;
    float4 SunDirectionAndIntensity;
    float4 CloudShadowParameters;
    float4 CloudShadowProjection;
};

float3 authoredSrgbToLinear(float3 color)
{
    float3 low = color / 12.92f;
    float3 high = pow((color + 0.055f) / 1.055f, 2.4f);
    return lerp(low, high, step(0.04045f, color));
}

float atmosphericHaze(float distanceToCamera, float cameraHeight)
{
    float cameraDensity = exp(-max(cameraHeight - AtmosphereParameters.z, 0.0f) * AtmosphereParameters.y);
    float opticalDepth = distanceToCamera * AtmosphereParameters.x * sqrt(max(cameraDensity, 0.0f));
    return saturate((1.0f - exp(-opticalDepth)) * AtmosphereParameters.w);
}

// Same world-anchored cloud-shadow field as the terrain and vegetation, so
// lakes darken under the clouds overhead. WorldXZ is relative to the floating
// origin, so shift it into absolute world coordinates first.
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

float cloudSunVisibility(float2 worldPosition, float distanceToCamera)
{
    float3 toSun = normalize(-SunDirectionAndIntensity.xyz);
    float travelToCloud = max(CloudShadowProjection.z / max(toSun.y, 0.08f), 0.0f);
    float2 cloudPosition = worldPosition + toSun.xz * travelToCloud;
    float time = CameraHeightFogTime.z;
    float2 wind = float2(0.84f, 0.54f) * time * CloudShadowParameters.w;
    float2 coordinate = (cloudPosition + wind) * CloudShadowParameters.z;
    float weather = cloudValueNoise(coordinate) * 0.72f
        + cloudValueNoise(coordinate * 2.07f + 37.1f + float2(-0.0009f, 0.0006f) * time) * 0.28f;
    float cells = cloudValueNoise(coordinate * 4.35f + float2(0.0029f, -0.0017f) * time + 5.83f);
    float threshold = 1.0f - CloudShadowParameters.x;
    float scatterGate = smoothstep(threshold - 0.20f, threshold + 0.04f, weather);
    float shape = saturate((weather - threshold) / max(CloudShadowParameters.x * 0.42f, 0.04f));
    shape = saturate(shape * lerp(0.38f, 1.38f, cells) + (cells - 0.70f) * 1.20f * scatterGate);
    float opacity = saturate(shape * CloudShadowParameters.y * 1.35f);
    opacity *= 1.0f - smoothstep(9000.0f, 36000.0f, distanceToCamera);
    return 1.0f - opacity * 0.50f;
}

float4 main(float2 worldXZ : TEXCOORD0, float distanceToCamera : TEXCOORD1) : SV_Target0
{
    float cloudVisibility = cloudSunVisibility(worldXZ + CloudShadowProjection.xy, distanceToCamera);
    float waveVisibility = (1.0f - smoothstep(700.0f, 7000.0f, CameraHeightFogTime.x))
        * (1.0f - smoothstep(1600.0f, 14000.0f, distanceToCamera));
    float phaseA = worldXZ.x * 0.021f + worldXZ.y * 0.009f + CameraHeightFogTime.z * 0.85f;
    float phaseB = worldXZ.x * -0.012f + worldXZ.y * 0.027f - CameraHeightFogTime.z * 0.61f;
    float2 noiseCoordinate = worldXZ * 0.018f
        + float2(0.23f, -0.17f) * CameraHeightFogTime.z;
    float noiseLeft = cloudValueNoise(noiseCoordinate - float2(0.35f, 0.0f));
    float noiseRight = cloudValueNoise(noiseCoordinate + float2(0.35f, 0.0f));
    float noiseDown = cloudValueNoise(noiseCoordinate - float2(0.0f, 0.35f));
    float noiseUp = cloudValueNoise(noiseCoordinate + float2(0.0f, 0.35f));
    float slopeX = (cos(phaseA) * 0.012f - cos(phaseB) * 0.007f
        + (noiseRight - noiseLeft) * 0.11f) * waveVisibility;
    float slopeZ = (cos(phaseA) * 0.005f + cos(phaseB) * 0.015f
        + (noiseUp - noiseDown) * 0.11f) * waveVisibility;
    float3 normal = normalize(float3(-slopeX * 5.0f, 1.0f, -slopeZ * 5.0f));
    float3 toCamera = normalize(float3(
        CameraHeightFogTime.y - worldXZ.x,
        max(CameraHeightFogTime.x, 1.68f),
        CameraHeightFogTime.w - worldXZ.y));
    float3 toSun = normalize(-SunDirectionAndIntensity.xyz);
    float fresnel = 0.025f + 0.975f * pow(1.0f - saturate(dot(normal, toCamera)), 5.0f);
    float sunGlint = pow(saturate(dot(reflect(-toSun, normal), toCamera)), 192.0f)
        * SunDirectionAndIntensity.w * cloudVisibility;
    float3 deepWater = authoredSrgbToLinear(float3(0.018f, 0.115f, 0.17f));
    float3 shallowWater = authoredSrgbToLinear(float3(0.075f, 0.31f, 0.36f));
    // Reflect the actual view direction through the animated normal. Sampling
    // the sky from normal.y alone was nearly constant and reduced the ocean to
    // a flat cyan sheet even when its wave normals changed.
    float3 reflectionDirection = reflect(-toCamera, normal);
    float reflectionHeight = smoothstep(-0.10f, 0.72f, reflectionDirection.y);
    float3 skyReflection = lerp(HorizonColor.rgb, ZenithColor.rgb, reflectionHeight)
        * lerp(0.60f, 1.0f, cloudVisibility);
    float distanceReflection = smoothstep(180.0f, 12000.0f, distanceToCamera);
    float3 body = lerp(shallowWater, deepWater, saturate(distanceToCamera / 4200.0f))
        * lerp(0.76f, 1.0f, cloudVisibility);
    float reflectionWeight = saturate(fresnel * 0.92f + distanceReflection * 0.16f);
    float3 water = body * (1.0f - reflectionWeight * 0.72f)
        + skyReflection * reflectionWeight;
    water += sunGlint * authoredSrgbToLinear(float3(1.0f, 0.84f, 0.58f));
    float haze = atmosphericHaze(distanceToCamera, CameraHeightFogTime.x);
    return float4(lerp(water, HorizonColor.rgb, saturate(haze)) * SkyLightingParameters.y, 1.0f);
}
