#version 450
// Vulkan 变体：RHI 贴图四边形 顶点着色器
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;

layout(push_constant) uniform PC {
    mat4 uMatrixMVP;
    vec4 uColor;
} pc;

layout(location = 0) out vec2 vUV;

void main()
{
    vUV = aUV;
    gl_Position = pc.uMatrixMVP * vec4(aPos, 1.0);
}
