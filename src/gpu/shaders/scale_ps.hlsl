// A texture's color times a factor, texel for texel (the reduced bloom setting).
struct PushConstants
{
    uint TextureIndex;
    float Factor;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

float4 main(in float4 position : SV_Position) : SV_Target
{
    float4 color = g_Texture2DDescriptorHeap[g_PushConstants.TextureIndex].Load(int3(position.xy, 0));
    return float4(color.rgb * g_PushConstants.Factor, color.a);
}
