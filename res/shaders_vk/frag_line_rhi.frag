#version 450
// Vulkan 变体：D1 宽线（RHI 展开路径）片元着色器。
// 与 GL 版 res/shaders/frag_line_rhi 语义一致：线色直出（sRGB 交换链下由
// 硬件编码，与 frag_color 同链路）。
layout(push_constant) uniform PC {
    mat4 uMatrixMVPPad;
    vec4 uColor;
} pc;

layout(location = 0) out vec4 FragColor;

void main()
{
    FragColor = vec4(pc.uColor.rgb, 1.0);
}
