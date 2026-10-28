// Fullscreen triangle: three vertices, no vertex buffer. Vulkan clip space
// (y = -1 at the top; internal shaders are compiled without -fvk-invert-y),
// texture coordinates (0, 0) at the top left.
void main(in uint vertexId : SV_VertexID, out float4 position : SV_Position, out float2 texCoord : TEXCOORD0)
{
    texCoord = float2((vertexId << 1) & 2, vertexId & 2);
    position = float4(texCoord * 2.0 - 1.0, 0.0, 1.0);
}
