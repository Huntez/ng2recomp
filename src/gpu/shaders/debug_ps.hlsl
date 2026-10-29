// NG2_DEBUG_HIGHLIGHT: draws of the highlighted vertex shaders in a solid
// color (SharedConstants.debugColor).
struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

float4 main() : SV_Target
{
    return vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1744, 0x10);
}
