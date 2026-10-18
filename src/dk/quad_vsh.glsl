#version 460

// The game's picture as a quad: rect = the top-left and bottom-right corners in clip space (+y is up).
layout (std140, binding = 0) uniform Quad
{
    vec4 rect;
} u;

layout (location = 0) out vec2 outTexCoord;

void main()
{
    vec2 p = vec2(float(gl_VertexID & 1), float((gl_VertexID >> 1) & 1));   // a strip: (0,0) (1,0) (0,1) (1,1)
    gl_Position = vec4(mix(u.rect.xy, u.rect.zw, p), 0.0, 1.0);
    outTexCoord = p;
}
