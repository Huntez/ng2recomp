// Xenos rectangle lists: each three vertices are three corners of a
// rectangle; the fourth is v1 + v2 - v0 for every output (as Xenia does).
//
// Translated vertex shaders write TEXCOORD0-7 (locations 0-7), COLOR0-1
// (8-9) and TEXCOORD8-15 (10-17); NG2's pixel shaders only read TEXCOORD0-7
// and COLOR0-1, and passing all 18 would exceed the 64 geometry input
// components some GPUs allow, so only those pass through.
struct Vertex
{
    float4 position : SV_Position;
    [[vk::location(0)]] float4 texCoord[8] : TEXCOORD0;
    [[vk::location(8)]] float4 color0 : COLOR0;
    [[vk::location(9)]] float4 color1 : COLOR1;
};

Vertex Fourth(Vertex corner, Vertex a, Vertex b)
{
    Vertex v;
    v.position = a.position + b.position - corner.position;
    [unroll] for (uint i = 0; i < 8; i++)
        v.texCoord[i] = a.texCoord[i] + b.texCoord[i] - corner.texCoord[i];
    v.color0 = a.color0 + b.color0 - corner.color0;
    v.color1 = a.color1 + b.color1 - corner.color1;
    return v;
}

[maxvertexcount(4)]
void main(triangle Vertex input[3], inout TriangleStream<Vertex> output)
{
    // The corner between the rectangle's two sides is the one opposite the
    // longest edge (the diagonal).
    float d01 = distance(input[0].position.xy / input[0].position.w, input[1].position.xy / input[1].position.w);
    float d02 = distance(input[0].position.xy / input[0].position.w, input[2].position.xy / input[2].position.w);
    float d12 = distance(input[1].position.xy / input[1].position.w, input[2].position.xy / input[2].position.w);
    uint corner = 0;
    if (d01 >= d02 && d01 >= d12)
        corner = 2;
    else if (d02 >= d01 && d02 >= d12)
        corner = 1;
    uint a = (corner + 1) % 3;
    uint b = (corner + 2) % 3;

    output.Append(input[a]);
    output.Append(input[corner]);
    output.Append(Fourth(input[corner], input[a], input[b]));
    output.Append(input[b]);
}
