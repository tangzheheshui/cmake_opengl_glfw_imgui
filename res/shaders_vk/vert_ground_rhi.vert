#version 450
// V3-20：阴影接收面（地面）：位置直接是世界坐标（±20 的平面）
layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PC {
    mat4 uViewProj;   // offset 0（渲染侧已做 Reversed-Z 修正）
    mat4 uLightMat;   // offset 64（渲染侧已修正）
} pc;

layout(location = 0) out vec3 vWorld;

void main()
{
    vWorld = aPos;
    gl_Position = pc.uViewProj * vec4(aPos, 1.0);
}
