cbuffer WaterVertexUniform : register(b0, space1)
{
    float4x4 ViewProjection;
    float4 CameraLocalAndTime;
    float4 WaterParameters;
};

struct Output
{
    float4 Position : SV_Position;
    float2 WorldXZ : TEXCOORD0;
    float Distance : TEXCOORD1;
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

Output main(float3 Position : TEXCOORD0)
{
    Output output;
    float2 worldXZ = CameraLocalAndTime.xz + Position.xz * WaterParameters.x;
    // Keep the geometric ocean strictly below sea level. Vertex-displaced
    // waves crossed low coastline triangles, and their planet-scale mesh
    // interpolation produced large far-distance artifacts. All wave motion is
    // now shading-only in the fragment shader, so water cannot eat terrain.
    float height = WaterParameters.y;
    float3 world = projectToPlanet(float3(worldXZ.x, height, worldXZ.y), CameraLocalAndTime.xz);
    output.Position = mul(ViewProjection, float4(world, 1.0f));
    output.WorldXZ = worldXZ;
    output.Distance = length(world - CameraLocalAndTime.xyz);
    return output;
}
