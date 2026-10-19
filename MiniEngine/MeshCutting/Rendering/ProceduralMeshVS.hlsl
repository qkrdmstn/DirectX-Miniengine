cbuffer Transform : register(b0)
{
    float4x4 World;
    float4x4 ViewProj;
};

struct Input
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float4 color : COLOR0;
};

struct Output
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
};

Output main(Input input)
{
    Output output;
    output.position = mul(ViewProj, mul(World, float4(input.position, 1.0)));
    output.color = input.color;
    return output;
}
