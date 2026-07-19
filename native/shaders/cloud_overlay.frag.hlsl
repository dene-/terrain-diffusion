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

// Shared 2D cloud field: this exact hash, octave mix, wind vector, evolution
// drift and shape mapping are mirrored by the terrain/vegetation/water
// cloud-shadow functions, so ground shadows track the clouds overhead.
float cloudShape2D(float2 worldXZ, float time, out float cells)
{
    float2 wind = float2(0.84f, 0.54f) * time * CloudNoise.z;
    float2 weatherCoordinate = (worldXZ + wind) / max(CloudNoise.x, 1.0f);
    // The second octave drifts against the first, so the weather pattern
    // continuously shears and reshapes instead of translating as a rigid mask.
    float weather = valueNoise(weatherCoordinate) * 0.72f
        + valueNoise(weatherCoordinate * 2.07f + 37.1f + float2(-0.0009f, 0.0006f) * time) * 0.28f;
    // A mid-scale cells octave breaks systems into individual clouds and seeds
    // scattered puffs along their fringes: the sky mixes broad decks, tight
    // clusters and isolated small cumulus instead of one uniform blanket.
    cells = valueNoise(weatherCoordinate * 4.35f + float2(0.0029f, -0.0017f) * time + 5.83f);
    float threshold = 1.0f - CloudLayer.z;
    float scatterGate = smoothstep(threshold - 0.20f, threshold + 0.04f, weather);
    float shape = saturate((weather - threshold) / max(CloudLayer.z * 0.42f, 0.04f));
    return saturate(shape * lerp(0.38f, 1.38f, cells)
        + (cells - 0.70f) * 1.20f * scatterGate);
}

float cloudDensity(float3 worldPosition)
{
    float layerDepth = max(CloudLayer.y - CloudLayer.x, 1.0f);
    float height01 = saturate((worldPosition.y - CloudLayer.x) / layerDepth);
    float time = SunDirectionAndTime.w;
    float cells;
    float shape = cloudShape2D(worldPosition.xz, time, cells);
    float2 wind = float2(0.84f, 0.54f) * time * CloudNoise.z;
    float2 weatherCoordinate = (worldPosition.xz + wind) / max(CloudNoise.x, 1.0f);

    // A 2D weather map only chooses where cloud systems exist. Two inexpensive
    // 3D value-noise scales shape macro towers and erode their edges; otherwise
    // every altitude is merely a shifted copy of the same flat mask. All of the
    // coordinates drift slowly over time, so clouds continuously grow, boil
    // and dissipate even when the wind is calm.
    float2 heightShear = (height01 - 0.42f) * float2(1.65f, -1.10f);
    float3 macroCoordinate = float3(
        weatherCoordinate * 1.28f + heightShear * 0.34f,
        height01 * 2.65f);
    float macroVolume = valueNoise3D(macroCoordinate + float3(13.7f, 4.2f, 2.9f)
        + float3(0.0021f, 0.0013f, 0.0019f) * time);
    // Keep only one trilinear 3-D lookup in the hot ray-march path. A warped
    // 2-D erosion octave supplies the small breakup: the vertical shear means
    // successive heights do not reuse one flat silhouette, while avoiding a
    // second eight-corner 3-D interpolation at every primary and light sample.
    float2 detailCoordinate = (worldPosition.xz + wind * 1.7f)
        / max(CloudNoise.y, 1.0f) + heightShear * 1.7f + macroVolume * 0.83f
        + float2(0.0061f, -0.0043f) * time;
    float detailVolume = valueNoise(detailCoordinate + 7.3f);
    shape = saturate(shape + (macroVolume - 0.50f) * 0.68f);
    float volumeNoise = macroVolume * 0.64f + detailVolume * 0.36f;
    float billow = smoothstep(0.24f, 0.77f, volumeNoise);
    // A constant geometric cloud base reads as a flat ceiling from the ground.
    // Vary the local base and cap with the same world-anchored volume fields so
    // the underside forms shelves and hanging billows without adding samples.
    // The cells octave links clump footprint to clump height, so large cells
    // tower while thin scraps stay flat.
    float localHeight = height01
        - (macroVolume - 0.50f) * 0.085f
        - (detailVolume - 0.50f) * 0.035f;
    float localTop = 0.76f + (macroVolume - 0.50f) * 0.22f + (cells - 0.50f) * 0.18f;
    float verticalProfile = smoothstep(-0.015f, 0.075f, localHeight)
        * (1.0f - smoothstep(localTop - 0.22f, localTop, localHeight));
    float verticalGrowth = lerp(0.72f, 1.18f, smoothstep(0.08f, 0.54f, height01));
    float erodedShape = shape * verticalGrowth * lerp(0.48f, 1.34f, billow)
        - (1.0f - billow) * 0.34f;
    // One fine octave, faded in only near the camera, wisps the edges of close
    // clouds at a few-hundred-metre scale and churns on a ~30 second cycle.
    // Distant and light-shaft samples skip it, so it costs nothing there and
    // cannot alias into far-field shimmer.
    float fineWeight = 1.0f - smoothstep(3000.0f, 16000.0f,
        distance(worldPosition, CameraPositionAndAltitude.xyz));
    if (fineWeight > 0.001f)
    {
        float fine = valueNoise(detailCoordinate * 6.3f
            + float2(-0.013f, 0.009f) * time + 2.17f);
        erodedShape = saturate(erodedShape + (fine - 0.5f) * 0.44f * fineWeight);
    }
    return saturate(erodedShape) * verticalProfile * CloudLayer.w;
}

float4 main(float2 uv : TEXCOORD0) : SV_Target0
{
    if (CameraUpAndOrbital.w >= 0.995f) return 0.0f;

    float2 screen = uv * 2.0f - 1.0f;
    float3 rayDirection = normalize(
        CameraForwardAndTanHalfFov.xyz
        + CameraRightAndAspect.xyz * screen.x * CameraForwardAndTanHalfFov.w * CameraRightAndAspect.w
        - CameraUpAndOrbital.xyz * screen.y * CameraForwardAndTanHalfFov.w);
    if (abs(rayDirection.y) < 0.0005f) return 0.0f;

    float lowerIntersection = (CloudLayer.x - CameraPositionAndAltitude.y) / rayDirection.y;
    float upperIntersection = (CloudLayer.y - CameraPositionAndAltitude.y) / rayDirection.y;
    float entryDistance = max(min(lowerIntersection, upperIntersection), 0.0f);
    float exitDistance = min(max(lowerIntersection, upperIntersection), 180000.0f);
    if (exitDistance <= entryDistance) return 0.0f;

    float cameraHeight01 = saturate((CameraPositionAndAltitude.y - CloudLayer.x)
        / max(CloudLayer.y - CloudLayer.x, 1.0f));
    bool cameraInsideCloudLayer = cameraHeight01 > 0.0f && cameraHeight01 < 1.0f;
    // A shallow ray can mathematically remain in the cloud slab for hundreds
    // of kilometres. Marching that entire interval creates visible altitude
    // slices and spends work on clouds already hidden by aerial perspective.
    // Real cloud extinction is local, so bound the in-cloud optical segment.
    if (cameraInsideCloudLayer)
    {
        exitDistance = min(exitDistance, entryDistance + 42000.0f);
    }

    int configuredSteps = clamp((int)round(CloudNoise.w), 8, 20);
    float targetSegmentLength = cameraInsideCloudLayer ? 2400.0f : 5200.0f;
    int distanceSteps = clamp((int)ceil((exitDistance - entryDistance) / targetSegmentLength), 8, 28);
    int stepCount = max(configuredSteps, distanceSteps);
    float marchDistance = exitDistance - entryDistance;
    // Concentrate samples close to a camera inside the layer. Equal-distance
    // slices across a near-horizontal 100+ km ray become visible as stacked
    // contours even with jitter; progressively wider segments remove that
    // regular spacing while preserving the bounded sample count.
    float distanceExponent = cameraInsideCloudLayer ? 1.55f : 1.18f;
    // A stable interleaved offset trades coherent ray-slice contours for
    // sub-pixel noise that the quarter-resolution linear upscale naturally
    // filters. It is deliberately not animated, avoiding temporal sparkle.
    float stepJitter = 0.12f + hash21(floor(uv * 2048.0f)) * 0.76f;
    float transmittance = 1.0f;
    float3 radiance = 0.0f;
    float3 toSun = normalize(-SunDirectionAndTime.xyz);
    float forwardScatter = pow(saturate(dot(rayDirection, toSun)), 8.0f);
    float cachedSunVisibility = 1.0f;
    [loop] for (int stepIndex = 0; stepIndex < 28; ++stepIndex)
    {
        if (stepIndex >= stepCount || transmittance < 0.025f) break;
        float segmentStart01 = pow((float)stepIndex / (float)stepCount, distanceExponent);
        float segmentEnd01 = pow((float)(stepIndex + 1) / (float)stepCount, distanceExponent);
        float segmentLength = max((segmentEnd01 - segmentStart01) * marchDistance, 1.0f);
        float distanceAlongRay = entryDistance
            + lerp(segmentStart01, segmentEnd01, stepJitter) * marchDistance;
        float3 samplePosition = CameraPositionAndAltitude.xyz + rayDirection * distanceAlongRay;
        float density = cloudDensity(samplePosition);
        float sampleOpacity = 1.0f - exp(-density * segmentLength * 0.00058f);
        if (stepIndex % 4 == 0)
        {
            float lightDensity = 0.0f;
            int lightSteps = clamp((int)round(CloudLight.x), 1, 2);
            [unroll] for (int lightIndex = 0; lightIndex < 2; ++lightIndex)
            {
                if (lightIndex >= lightSteps) break;
                lightDensity += cloudDensity(samplePosition + toSun * (650.0f + (float)lightIndex * 900.0f));
            }
            cachedSunVisibility = exp(-lightDensity * 0.72f);
        }
        float height01 = saturate((samplePosition.y - CloudLayer.x) / max(CloudLayer.y - CloudLayer.x, 1.0f));
        float selfShadow = cachedSunVisibility * exp(-density * lerp(1.25f, 0.32f, height01));
        float sunLight = lerp(0.40f, 1.0f, selfShadow) + forwardScatter * 0.38f;
        // The sky dome reaches cloud tops far more than their bases, so a
        // height-graded ambient keeps undersides murky and crowns bright.
        // Without it every cloud reads as a uniformly lit flat wisp.
        float ambientGrade = lerp(0.62f, 1.06f, height01);
        float3 sampleColor = lerp(
            authoredSrgbToLinear(float3(0.43f, 0.49f, 0.55f)),
            authoredSrgbToLinear(float3(1.0f, 0.94f, 0.84f)), sunLight) * ambientGrade;
        radiance += transmittance * sampleOpacity * sampleColor;
        transmittance *= 1.0f - sampleOpacity;
    }
    float opacity = 1.0f - transmittance;
    float elevation = saturate(rayDirection.y);
    float3 skyRadiance = lerp(HorizonColor.rgb, ZenithColor.rgb, pow(elevation, 0.56f));
    float distanceHaze = smoothstep(18000.0f, 105000.0f, entryDistance);
    radiance = lerp(radiance, skyRadiance * opacity, distanceHaze * 0.90f);
    if (cameraInsideCloudLayer)
    {
        // Near-horizontal in-layer rays are the pathological slab case: even a
        // bounded march exposes its discrete height samples. Represent the
        // immediate medium with one world-space 3-D density lookup, then blend
        // back to the full volume as the ray becomes vertical. This reads as
        // local cloud fog instead of repeated horizontal sheets.
        float localDensity = cloudDensity(
            CameraPositionAndAltitude.xyz + rayDirection * 1350.0f);
        float localOpacity = 1.0f - exp(-localDensity * 2.4f);
        float3 localColor = authoredSrgbToLinear(float3(0.66f, 0.72f, 0.77f));
        float fullVolumeBlend = smoothstep(0.055f, 0.22f, abs(rayDirection.y));
        radiance = lerp(localColor * localOpacity, radiance, fullVolumeBlend);
        opacity = lerp(localOpacity, opacity, fullVolumeBlend);
    }
    return float4(radiance * CloudLight.z, opacity);
}
