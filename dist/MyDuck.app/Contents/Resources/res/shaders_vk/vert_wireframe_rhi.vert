#version 450
// Vulkan 变体：线框顶点（PC：mat4 uViewProj@0 + vec4 uClipPlane@64 + vec4 uColor@80）
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec4 iRow0;
layout(location = 4) in vec4 iRow1;
layout(location = 5) in vec4 iRow2;
layout(location = 6) in vec4 iRow3;

layout(push_constant) uniform PC {
    mat4 uViewProj;
    vec4 uClipPlane;
    vec4 uColor;     // offset 80：线框颜色（片元消费）
} pc;

layout(location = 0) out float vClipDistance;

void main()
{
    mat4 aInstanceMatrix = mat4(iRow0, iRow1, iRow2, iRow3);
    vec3 worldPos = (aInstanceMatrix * vec4(aPos, 1.0)).xyz;
    vClipDistance = dot(vec4(worldPos, 1.0), pc.uClipPlane);
    gl_Position = pc.uViewProj * aInstanceMatrix * vec4(aPos, 1.0);
}
