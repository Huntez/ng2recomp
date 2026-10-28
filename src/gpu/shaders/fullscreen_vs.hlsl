// Fullscreen triangle: three vertices, no vertex buffer.
void main(in uint vertexId : SV_VertexID, out float4 position : SV_Position, out float2 texCoord : TEXCOORD0)
{
    texCoord = float2((vertexId << 1) & 2, vertexId & 2);
    position = float4(texCoord * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
