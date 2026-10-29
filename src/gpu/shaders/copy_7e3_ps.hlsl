// Resolve of a 2_10_10_10_FLOAT (7e3) render target: the EDRAM holds 10-bit
// 7e3 color and 2-bit alpha, which the host's float16 target doesn't. Rounds
// the source texel the way packing it into EDRAM would, so passes reading the
// resolve (bloom, tone mapping) see what they would on the console.
#include "float7e3.hlsli"

struct PushConstants
{
    uint TextureIndex;
    int OffsetX;
    int OffsetY;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

float4 main(in float4 position : SV_Position) : SV_Target
{
    int2 texel = int2(position.xy) + int2(g_PushConstants.OffsetX, g_PushConstants.OffsetY);
    return Round7e3(g_Texture2DDescriptorHeap[g_PushConstants.TextureIndex].Load(int3(texel, 0)));
}
