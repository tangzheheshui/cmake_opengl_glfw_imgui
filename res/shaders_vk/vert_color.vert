#version 450
// Vulkan 变体：纯色顶点着色器（Line 等使用，与 GL 版 vert_color 对应）
layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PC {
    mat4 uMatrixMVP;
    vec4 uColor;
} pc;

void main()
{
    gl_Position = pc.uMatrixMVP * vec4(aPos, 1.0);
}
