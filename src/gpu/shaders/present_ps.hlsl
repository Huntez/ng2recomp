// Scales the guest front buffer to the window (linear filtering), then
// applies the display's gamma table (UpdateGammaTable) as scan-out would.
// Push constants share the 24-byte range the game's shaders use for buffer
// addresses; internal shaders only use the first bytes.
struct PushConstants
{
    uint TextureIndex;
    uint SamplerIndex;
    uint GammaTableIndex;  // 256x1, 0 = none
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

// The table maps 8-bit front buffer values; between entries (after the
// scaling filter), interpolate.
float ApplyGamma(Texture2D<float4> table, float value, uint channel)
{
    float position = value > 0.0 ? min(value, 1.0) * 255.0 : 0.0;  // NaN to 0
    int index = min(int(position), 254);
    float low = table.Load(int3(index, 0, 0))[channel];
    float high = table.Load(int3(index + 1, 0, 0))[channel];
    return lerp(low, high, position - float(index));
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD0) : SV_Target
{
    float4 color = g_Texture2DDescriptorHeap[g_PushConstants.TextureIndex].Sample(
        g_SamplerDescriptorHeap[g_PushConstants.SamplerIndex], texCoord);
    if (g_PushConstants.GammaTableIndex != 0)
    {
        Texture2D<float4> table = g_Texture2DDescriptorHeap[g_PushConstants.GammaTableIndex];
        color.r = ApplyGamma(table, color.r, 0);
        color.g = ApplyGamma(table, color.g, 1);
        color.b = ApplyGamma(table, color.b, 2);
    }
    return float4(color.rgb, 1.0);
}
