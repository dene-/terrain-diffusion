cbuffer SkyUniform : register(b0, space3)
{
    float4 SunDirectionAndTime;
    float4 HorizonColor;
    float4 ZenithColor;
    float4 CameraPositionAndAltitude;
    float4 CameraForwardAndTanHalfFov;
    float4 CameraRightAndAspect;
    float4 CameraUpAndOrbital;
    float4 CloudLayer;
    float4 CloudNoise;
    float4 CloudLight;
};

float hash21(float2 position)
{
    position = frac(position * float2(123.34f, 345.45f));
    position += dot(position, position + 34.345f);
    return frac(position.x * position.y);
}

float valueNoise(float2 position)
{
    float2 cell = floor(position);
    float2 local = frac(position);
    local = local * local * (3.0f - 2.0f * local);
    return lerp(lerp(hash21(cell), hash21(cell + float2(1, 0)), local.x),
                lerp(hash21(cell + float2(0, 1)), hash21(cell + 1), local.x), local.y);
}

float3 authoredSrgbToLinear(float3 color)
{
    float3 low = color / 12.92f;
    float3 high = pow((color + 0.055f) / 1.055f, 2.4f);
    return lerp(low, high, step(0.04045f, color));
}

float hash31(float3 position)
{
    position = frac(position * 0.1031f);
    position += dot(position, position.yzx + 33.33f);
    return frac((position.x + position.y) * position.z);
}

float valueNoise3D(float3 position)
{
    float3 cell = floor(position);
    float3 local = frac(position);
    local = local * local * (3.0f - 2.0f * local);
    float lower = lerp(
        lerp(hash31(cell), hash31(cell + float3(1, 0, 0)), local.x),
        lerp(hash31(cell + float3(0, 1, 0)), hash31(cell + float3(1, 1, 0)), local.x), local.y);
    float upper = lerp(
        lerp(hash31(cell + float3(0, 0, 1)), hash31(cell + float3(1, 0, 1)), local.x),
        lerp(hash31(cell + float3(0, 1, 1)), hash31(cell + 1), local.x), local.y);
    return lerp(lower, upper, local.z);
}

float cloudDensity(float3 worldPosition)
{
    float layerDepth = max(CloudLayer.y - CloudLayer.x, 1.0f);
    float height01 = saturate((worldPosition.y - CloudLayer.x) / layerDepth);
    float verticalProfile = smoothstep(0.0f, 0.09f, height01)
        * (1.0f - smoothstep(0.58f, 1.0f, height01));
    float2 wind = float2(0.84f, 0.54f) * SunDirectionAndTime.w * CloudNoise.z;
    float2 weatherCoordinate = (worldPosition.xz + wind) / max(CloudNoise.x, 1.0f);
    float weather = valueNoise(weatherCoordinate) * 0.72f
        + valueNoise(weatherCoordinate * 2.07f + 37.1f) * 0.28f;
    float coverageThreshold = 1.0f - CloudLayer.z;
    float cloudShape = saturate((weather - coverageThreshold) / max(CloudLayer.z * 0.42f, 0.04f));
    float3 detailCoordinate = float3(
        (worldPosition.xz + wind * 1.7f) / max(CloudNoise.y, 1.0f),
        height01 * 3.4f);
    float detail = valueNoise3D(detailCoordinate + float3(0.0f, 0.0f, 7.3f));
    float erosion = smoothstep(0.26f, 0.78f, detail);
    float heightShape = 1.0f - abs(height01 - lerp(0.30f, 0.48f, weather)) * 0.24f;
    float erodedShape = saturate(cloudShape * heightShape - (1.0f - erosion) * 0.66f);
    return erodedShape * verticalProfile * CloudLayer.w;
}

float3 baseSky(float3 rayDirection)
{
    float elevation = saturate(rayDirection.y);
    float horizonGlow = exp(-abs(rayDirection.y) * lerp(6.0f, 3.8f, saturate((CloudLight.y - 1.5f) / 5.0f)));
    float3 sky = lerp(HorizonColor.rgb, ZenithColor.rgb, pow(elevation, 0.56f));
    sky += HorizonColor.rgb * horizonGlow * 0.12f;
    float3 toSun = normalize(-SunDirectionAndTime.xyz);
    float sunCosine = dot(rayDirection, toSun);
    float disk = smoothstep(0.99978f, 0.99994f, sunCosine);
    float aureole = pow(saturate(sunCosine), 192.0f) * 0.38f;
    sky += authoredSrgbToLinear(float3(1.0f, 0.82f, 0.55f)) * (disk * 5.2f + aureole);
    return sky;
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    float2 screen = uv * 2.0f - 1.0f;
    float3 rayDirection = normalize(
        CameraForwardAndTanHalfFov.xyz
        + CameraRightAndAspect.xyz * screen.x * CameraForwardAndTanHalfFov.w * CameraRightAndAspect.w
        - CameraUpAndOrbital.xyz * screen.y * CameraForwardAndTanHalfFov.w);
    float3 sky = baseSky(rayDirection);

    sky *= CloudLight.z;
    return float4(sky, 1.0f);
}
