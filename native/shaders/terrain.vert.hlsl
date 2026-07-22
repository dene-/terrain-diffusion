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

Output main(Input input)
{
    Output output;
    float3 flatWorld = input.Position + TileOffsetAndLevel.xyz;
    float2 cameraDelta = flatWorld.xz - CameraLocalAndTime.xz;
    // Keep successively coarser parents just beneath their children. They
    // remain a complete fallback without creating coincident-depth cracks.
    flatWorld.y -= TileOffsetAndLevel.w * 0.45f;
    float3 world = flatWorld;
    output.Position = mul(ViewProjection, float4(world, 1.0f));
    output.LocalWorld = flatWorld;
    output.Normal = input.Normal;
    output.Color = input.Color;
    output.Derived = input.Derived;
    output.Biome = input.Biome;
    output.LodLevel = TileOffsetAndLevel.w;
    output.TileCoordinates = input.Position.xz / max(NoiseOriginZAndPadding.y, 1.0f);
    output.ShadowPosition = mul(ShadowViewProjection, float4(world, 1.0f));
    output.FarShadowPosition = mul(FarShadowViewProjection, float4(world, 1.0f));
    output.DistanceAndRadii = float4(length(world - CameraLocalAndTime.xyz), length(cameraDelta),
        LodRadiiAndNoiseOriginX.x, LodRadiiAndNoiseOriginX.y);
    output.ViewDirection = normalize(world - CameraLocalAndTime.xyz);
    output.NoisePosition = flatWorld.xz + float2(LodRadiiAndNoiseOriginX.w, NoiseOriginZAndPadding.x);
    return output;
}
