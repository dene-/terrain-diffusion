cbuffer VegetationVertexUniform : register(b0, space1)
{
    float4x4 ViewProjection;
    float4x4 ShadowViewProjection;
    float4 TileOffsetAndLevel;
    float4 CameraLocalAndTime;
    float4 DrawParameters;
    float4 LodRadiiAndType;
};

static const float EarthRadius = 6371000.0f;

float3 projectToPlanet(float3 flatWorld, float2 cameraXZ)
{
    float2 delta = flatWorld.xz - cameraXZ;
    float arc = length(delta);
    if (arc < 0.01f) return flatWorld;
    float2 direction = delta / arc;
    float angle = arc / EarthRadius;
    float radial = EarthRadius + flatWorld.y;
    float2 projectedXZ = cameraXZ + direction * (radial * sin(angle));
    return float3(projectedXZ.x, radial * cos(angle) - EarthRadius, projectedXZ.y);
}

struct Input
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 VertexColor : TEXCOORD2;
    float3 InstancePosition : TEXCOORD3;
    float InstanceScale : TEXCOORD4;
    float4 InstanceColor : TEXCOORD5;
    float2 InstanceParameters : TEXCOORD6;
    float2 UV : TEXCOORD7;
};

struct Output
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

float2 treeDimensions(float species)
{
    if (species < 0.5f) return float2(8.8f, 22.0f);  // spruce
    if (species < 1.5f) return float2(8.2f, 18.5f);  // birch
    if (species < 2.5f) return float2(13.5f, 18.0f); // oak
    if (species < 3.5f) return float2(15.5f, 13.0f); // acacia
    return float2(12.5f, 19.5f);                     // tropical
}

float stableHash(float2 position)
{
    float3 value = frac(float3(position.xyx) * 0.1031f);
    value += dot(value, value.yzx + 33.33f);
    return frac((value.x + value.y) * value.z);
}

Output main(Input input)
{
    Output output;
    const float type = LodRadiiAndType.z;
    const float angle = input.InstanceParameters.y;
    const float cosine = cos(angle);
    const float sine = sin(angle);
    float3 origin = input.InstancePosition + TileOffsetAndLevel.xyz;
    float2 cameraDelta = origin.xz - CameraLocalAndTime.xz;
    const float distance = length(cameraDelta);
    float3 local;
    float3 normal = input.Normal;
    float treeHeight = 0.0f;

    if (type < 0.5f || type > 1.5f)
    {
        const float species = clamp(floor(input.InstanceParameters.x + 0.5f), 0.0f, 4.0f);
        const float2 dimensions = treeDimensions(species) * input.InstanceScale * DrawParameters.x;
        treeHeight = dimensions.y;
        local = float3(input.Position.x * dimensions.x, input.Position.y * dimensions.y, input.Position.z * dimensions.x);
        if (type > 1.5f)
        {
            float3 toViewer = normalize(float3(-cameraDelta.x, 0.0f, -cameraDelta.y) + float3(0.0001f, 0.0f, 0.0f));
            float3 right = normalize(cross(float3(0.0f, 1.0f, 0.0f), toViewer));
            local = right * local.x + float3(0.0f, local.y, 0.0f);
            normal = toViewer;
        }
        else
        {
            local.xz = float2(local.x * cosine - local.z * sine, local.x * sine + local.z * cosine);
            normal.xz = float2(normal.x * cosine - normal.z * sine, normal.x * sine + normal.z * cosine);
        }
        output.UV = float2((species + input.UV.x) * 0.2f, input.UV.y);
    }
    else
    {
        local = float3(input.Position.x * DrawParameters.x, input.Position.y * input.InstanceScale,
            input.Position.z * DrawParameters.x);
        float windWeight = smoothstep(0.05f, 1.0f, input.Position.y);
        float phase = origin.x * 0.075f + origin.z * 0.052f + CameraLocalAndTime.w * 1.65f;
        float gust = sin(phase) + sin(phase * 0.37f + CameraLocalAndTime.w * 0.72f) * 0.34f;
        local.x += gust * windWeight * DrawParameters.y;
        local.z += cos(phase * 0.79f) * windWeight * DrawParameters.y * 0.7f;
        local.xz = float2(local.x * cosine - local.z * sine, local.x * sine + local.z * cosine);
        normal.xz = float2(normal.x * cosine - normal.z * sine, normal.x * sine + normal.z * cosine);
        output.UV = input.UV;
    }

    float3 world = projectToPlanet(local + origin, CameraLocalAndTime.xz);
    float viewDistance = length(world - CameraLocalAndTime.xyz);
    output.Position = mul(ViewProjection, float4(world, 1.0f));
    output.Normal = normal;
    output.Color = input.VertexColor * input.InstanceColor;
    output.ShadowPosition = mul(ShadowViewProjection, float4(world, 1.0f));
    output.Distance = viewDistance;
    output.Type = type;
    float rangeVisibility = viewDistance >= LodRadiiAndType.x && viewDistance <= LodRadiiAndType.y ? 1.0f : 0.0f;
    float pixelVisibility = 1.0f;
    if (type < 0.5f || type > 1.5f)
    {
        float projectedPixels = treeHeight * DrawParameters.z / max(viewDistance, 1.0f);
        float geometryFade = smoothstep(DrawParameters.w * 0.78f,
            DrawParameters.w * 1.22f, projectedPixels);
        float sparseKeep = lerp(0.16f, 1.0f,
            smoothstep(DrawParameters.w, max(LodRadiiAndType.w, DrawParameters.w + 0.01f), projectedPixels));
        float random = stableHash(floor(origin.xz * 0.25f));
        pixelVisibility = random <= geometryFade * sparseKeep ? 1.0f : 0.0f;
    }
    output.Visibility = rangeVisibility * pixelVisibility;
    output.SurfaceHeight = origin.y;
    output.ViewDirection = normalize(world - CameraLocalAndTime.xyz);
    return output;
}
