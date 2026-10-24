cbuffer Constants : register(b0)
{
    float4x4 ViewProjection;
};

struct Vertex
{
    float4 Position : SV_Position;
    float4 Color : COLOR;
};

// 월드 정점을 카메라 행렬로 투영하고 선 색을 전달한다.
Vertex main(float3 position : POSITION, float4 color : COLOR)
{
    Vertex output;
    output.Position = mul(ViewProjection, float4(position, 1));
    output.Color = color;
    return output;
}
