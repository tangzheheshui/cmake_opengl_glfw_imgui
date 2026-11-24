#version 450
// Vulkan 变体：RHI 贴图四边形 片元着色器（set0/binding0 = 槽 0 纹理）
layout(set = 1, binding = 0) uniform sampler2D uTex;   // V3-2：槽 0 = set1

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 FragColor;

void main()
{
    FragColor = texture(uTex, vUV);
}
