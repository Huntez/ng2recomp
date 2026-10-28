// Scales the guest front buffer to the window (linear filtering).
// Push constants share the 24-byte range the game's shaders use for buffer
// addresses; internal shaders only use the first bytes.
struct PushConstants
{
    uint TextureIndex;
    uint SamplerIndex;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD0) : SV_Target
{
    float4 color = g_Texture2DDescriptorHeap[g_PushConstants.TextureIndex].Sample(
        g_SamplerDescriptorHeap[g_PushConstants.SamplerIndex], texCoord);
    return float4(color.rgb, 1.0);
}
