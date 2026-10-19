cbuffer SectionTint : register(b1)
{
    float4 TintColor;
    uint TintMode;      // 0 = vertex color, 1 = fixed section color
};

struct Input
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
};

float4 main(Input input) : SV_TARGET
{
    return TintMode != 0 ? float4(TintColor.rgb, 1.0) : input.color;
}
