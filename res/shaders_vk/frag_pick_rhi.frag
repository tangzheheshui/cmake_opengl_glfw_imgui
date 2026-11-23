#version 450
// Vulkan 变体：RHI 拾取 片元着色器（D2）
// ID 渲染直出：uPickColor 由 CPU 侧从对象 ID 编码（r = id&0xFF / g = id>>8）
layout(push_constant) uniform PC {
    mat4 uViewProj;    // offset 0（顶点用，此处占位保持布局一致）
    vec4 uPickColor;   // offset 64
} pc;

layout(location = 0) out vec4 FragColor;

void main()
{
    FragColor = pc.uPickColor;
}
