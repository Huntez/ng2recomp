// Resolve with format conversion: the destination pixel takes the source
// texel at its position plus an offset (source rect origin - destination).
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
    return g_Texture2DDescriptorHeap[g_PushConstants.TextureIndex].Load(int3(texel, 0));
}
