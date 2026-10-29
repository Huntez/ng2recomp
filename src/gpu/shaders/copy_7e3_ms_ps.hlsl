// copy_7e3_ps from a multisampled target: each sample is 7e3 in EDRAM before
// the resolve averages them, so round every sample, then average.
#include "float7e3.hlsli"

struct PushConstants
{
    uint TextureIndex;
    int OffsetX;
    int OffsetY;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

Texture2DMS<float4> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

float4 main(in float4 position : SV_Position) : SV_Target
{
    int2 texel = int2(position.xy) + int2(g_PushConstants.OffsetX, g_PushConstants.OffsetY);
    Texture2DMS<float4> source = g_Texture2DMSDescriptorHeap[g_PushConstants.TextureIndex];
    uint width, height, samples;
    source.GetDimensions(width, height, samples);
    float4 sum = 0.0;
    for (uint i = 0; i < samples; i++)
    {
        sum += Round7e3(source.Load(texel, i));
    }
    return sum / float(samples);
}
