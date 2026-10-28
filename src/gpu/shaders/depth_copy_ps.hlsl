// Depth resolve: the destination (R32_FLOAT, sampled by the game as a 24_8
// texture) takes the depth at its position plus an offset (source rect
// origin - destination).
struct PushConstants
{
    uint TextureIndex;
    int OffsetX;
    int OffsetY;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

Texture2D<float> g_Texture2DDescriptorHeap[] : register(t0, space0);

float main(in float4 position : SV_Position) : SV_Target
{
    int2 texel = int2(position.xy) + int2(g_PushConstants.OffsetX, g_PushConstants.OffsetY);
    return g_Texture2DDescriptorHeap[g_PushConstants.TextureIndex].Load(int3(texel, 0));
}
