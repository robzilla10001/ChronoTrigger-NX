#version 460

// Dear ImGui's vertices: pixel positions, scaled and moved to clip space by the uniform (xy scale, zw translate).
layout (location = 0) in vec2 inPos;
layout (location = 1) in vec2 inTexCoord;
layout (location = 2) in vec4 inColor;

layout (std140, binding = 0) uniform Gui
{
    vec4 scaleTrans;
} u;

layout (location = 0) out vec2 outTexCoord;
layout (location = 1) out vec4 outColor;

void main()
{
    gl_Position = vec4(inPos * u.scaleTrans.xy + u.scaleTrans.zw, 0.0, 1.0);
    outTexCoord = inTexCoord;
    outColor = inColor;
}
