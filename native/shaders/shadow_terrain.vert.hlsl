cbuffer ShadowUniform : register(b0, space1)
{
    float4x4 ShadowViewProjection;
    float4 TileOffset;
    float4 CameraLocal;
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

float4 main(float3 Position : TEXCOORD0) : SV_Position
{
    float3 flatWorld = Position + TileOffset.xyz;
    flatWorld.y -= TileOffset.w * 0.45f;
    return mul(ShadowViewProjection,
        float4(projectToPlanet(flatWorld, CameraLocal.xz), 1.0f));
}
