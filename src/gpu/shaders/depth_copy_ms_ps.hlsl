// Depth resolve from a multisampled depth buffer: depth can't go through a
// hardware resolve, so take sample 0 (Xenos resolves depth the same way).
struct PushConstants
{
    uint TextureIndex;
    int OffsetX;
    int OffsetY;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

Texture2DMS<float> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

float main(in float4 position : SV_Position) : SV_Target
{
    int2 texel = int2(position.xy) + int2(g_PushConstants.OffsetX, g_PushConstants.OffsetY);
    return g_Texture2DMSDescriptorHeap[g_PushConstants.TextureIndex].Load(texel, 0);
}
