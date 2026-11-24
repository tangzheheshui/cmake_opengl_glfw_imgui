#version 450
// Vulkan 变体：SDF 文字 顶点着色器（PC：mat4 uMatrixMVP@0 + vec4 uColor@64）
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(push_constant) uniform PC {
    mat4 uMatrixMVP;
    vec4 uColor;
} pc;

layout(location = 0) out vec2 vTexCoord;

void main()
{
    vTexCoord = aTexCoord;
    gl_Position = pc.uMatrixMVP * vec4(aPos, 1.0);
}
