cbuffer ShadowVegetationUniform : register(b0, space1)
{
    float4x4 ShadowViewProjection;
    float4 TileOffset;
    float4 DrawParameters;
    float4 CameraLocal;
};

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
    float2 UV : TEXCOORD0;
};

float2 treeDimensions(float species)
{
    if (species < 0.5f) return float2(8.8f, 22.0f);
    if (species < 1.5f) return float2(8.2f, 18.5f);
    if (species < 2.5f) return float2(13.5f, 18.0f);
    if (species < 3.5f) return float2(15.5f, 13.0f);
    return float2(12.5f, 19.5f);
}

Output main(Input input)
{
    Output output;
    float species = clamp(floor(input.InstanceParameters.x + 0.5f), 0.0f, 4.0f);
    float2 dimensions = treeDimensions(species) * input.InstanceScale * DrawParameters.x;
    float angle = input.InstanceParameters.y;
    float cosine = cos(angle);
    float sine = sin(angle);
    float3 local = float3(input.Position.x * dimensions.x, input.Position.y * dimensions.y, input.Position.z * dimensions.x);
    local.xz = float2(local.x * cosine - local.z * sine, local.x * sine + local.z * cosine);
    float3 flatWorld = local + input.InstancePosition + TileOffset.xyz;
    output.Position = mul(ShadowViewProjection, float4(flatWorld, 1.0f));
    output.UV = float2((species + input.UV.x) * 0.2f, input.UV.y);
    return output;
}
