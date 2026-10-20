cbuffer SectionTint : register(b1)
{
    float4 TintColor;
    uint TintMode;      // 0 = vertex color, 1 = solid section color, 2 = wire color, 3 = wire color by section
    uint SectionIndex;
};

// Bright colors for wireframe sections: original surface, surface split by a cut, cap, spare.
static const float3 kWirePalette[4] =
{
    float3(0.95, 0.95, 1.00),
    float3(1.00, 0.55, 0.10),
    float3(0.20, 1.00, 0.85),
    float3(1.00, 0.30, 0.80)
};

struct Input
{
    float4 position : SV_POSITION;
    float4 color : COLOR0;
};

float4 main(Input input) : SV_TARGET
{
    if (TintMode == 1 || TintMode == 2)
        return float4(TintColor.rgb, 1.0);
    if (TintMode == 3)
        return float4(kWirePalette[SectionIndex & 3], 1.0);
    return input.color;
}
