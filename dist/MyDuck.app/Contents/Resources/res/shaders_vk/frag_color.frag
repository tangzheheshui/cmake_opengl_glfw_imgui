#version 450
// Vulkan 变体：纯色片元着色器（与 GL 版 frag_color 对应）
layout(push_constant) uniform PC {
    mat4 uMatrixMVPPad;
    vec4 uColor;
} pc;

layout(location = 0) out vec4 FragColor;

void main()
{
    FragColor = vec4(pc.uColor.rgb, 1.0);
}
