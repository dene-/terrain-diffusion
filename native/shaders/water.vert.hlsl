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

Output main(float3 Position : TEXCOORD0)
{
    Output output;
    float2 worldXZ = CameraLocalAndTime.xz + Position.xz * WaterParameters.x;
    // Keep the geometric ocean strictly below sea level. Vertex-displaced
    // waves crossed low coastline triangles, and their large-scale mesh
    // interpolation produced large far-distance artifacts. All wave motion is
    // now shading-only in the fragment shader, so water cannot eat terrain.
    float height = WaterParameters.y;
    float3 world = float3(worldXZ.x, height, worldXZ.y);
    output.Position = mul(ViewProjection, float4(world, 1.0f));
    output.WorldXZ = worldXZ;
    output.Distance = length(world - CameraLocalAndTime.xyz);
    return output;
}
