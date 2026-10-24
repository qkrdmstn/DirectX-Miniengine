struct Vertex
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
};

Vertex main(float2 position : POSITION, float4 color : COLOR)
{
    Vertex output;
    output.Position = float4(position, 0.0, 1.0);
    output.Color = color;
    return output;
}
