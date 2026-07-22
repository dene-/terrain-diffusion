cbuffer ShadowUniform : register(b0, space1)
{
    float4x4 ShadowViewProjection;
    float4 TileOffset;
    float4 CameraLocal;
};

float4 main(float3 Position : TEXCOORD0) : SV_Position
{
    float3 flatWorld = Position + TileOffset.xyz;
    flatWorld.y -= TileOffset.w * 0.45f;
    return mul(ShadowViewProjection, float4(flatWorld, 1.0f));
}
