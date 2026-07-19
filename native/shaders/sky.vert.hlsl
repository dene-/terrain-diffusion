struct Output
{
    float4 Position : SV_Position;
    float2 UV : TEXCOORD0;
};

Output main(uint vertexId : SV_VertexID)
{
    Output output;
    output.UV = float2((vertexId << 1) & 2, vertexId & 2);
    output.Position = float4(output.UV * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.99999f, 1.0f);
    return output;
}

