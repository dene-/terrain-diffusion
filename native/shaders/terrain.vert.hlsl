cbuffer TerrainVertexUniform : register(b0, space1)
{
    float4x4 ViewProjection;
    float4x4 ShadowViewProjection;
    float4x4 FarShadowViewProjection;
    float4 TileOffsetAndLevel;
    float4 CameraLocalAndTime;
    float4 LodRadiiAndNoiseOriginX;
    float4 NoiseOriginZAndPadding;
};

struct Input
{
    float3 Position : TEXCOORD0;
    float3 Normal : TEXCOORD1;
    float4 Color : TEXCOORD2;
    float4 Derived : TEXCOORD3;
    float4 Biome : TEXCOORD4;
};

struct Output
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

float3 projectNormalToPlanet(float3 flatNormal, float2 delta)
{
    float arc = length(delta);
    if (arc < 0.01f) return flatNormal;
    float2 direction = delta / arc;
    float2 azimuth = float2(-direction.y, direction.x);
    float angle = arc / EarthRadius;
    float sine = sin(angle);
    float cosine = cos(angle);
    float3 radial = float3(direction.x * sine, cosine, direction.y * sine);
    float3 meridian = float3(direction.x * cosine, -sine, direction.y * cosine);
    float3 around = float3(azimuth.x, 0.0f, azimuth.y);
    return normalize(radial * flatNormal.y
        + meridian * dot(flatNormal.xz, direction)
        + around * dot(flatNormal.xz, azimuth));
}

Output main(Input input)
{
    Output output;
    float3 flatWorld = input.Position + TileOffsetAndLevel.xyz;
    float2 cameraDelta = flatWorld.xz - CameraLocalAndTime.xz;
    // Keep successively coarser parents just beneath their children. They
    // remain a complete fallback without creating coincident-depth cracks.
    flatWorld.y -= TileOffsetAndLevel.w * 0.45f;
    float3 world = projectToPlanet(flatWorld, CameraLocalAndTime.xz);
    output.Position = mul(ViewProjection, float4(world, 1.0f));
    output.LocalWorld = flatWorld;
    output.Normal = projectNormalToPlanet(input.Normal, cameraDelta);
    output.Color = input.Color;
    output.Derived = input.Derived;
    output.Biome = input.Biome;
    output.LodLevel = TileOffsetAndLevel.w;
    output.TileCoordinates = input.Position.xz / max(NoiseOriginZAndPadding.y, 1.0f);
    // Shadow casters use this same planet-projected position. Sampling a flat
    // shadow surface from curved visible geometry displaced shadows by an
    // increasing amount with distance and made them effectively disappear.
    output.ShadowPosition = mul(ShadowViewProjection, float4(world, 1.0f));
    output.FarShadowPosition = mul(FarShadowViewProjection, float4(world, 1.0f));
    output.DistanceAndRadii = float4(length(world - CameraLocalAndTime.xyz), length(cameraDelta),
        LodRadiiAndNoiseOriginX.x, LodRadiiAndNoiseOriginX.y);
    output.ViewDirection = normalize(world - CameraLocalAndTime.xyz);
    output.NoisePosition = flatWorld.xz + float2(LodRadiiAndNoiseOriginX.w, NoiseOriginZAndPadding.x);
    return output;
}
