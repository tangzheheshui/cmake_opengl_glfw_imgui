#version 450
// Vulkan 变体：RHI 贴图四边形 片元着色器（set0/binding0 = 槽 0 纹理）
layout(set = 0, binding = 1) uniform sampler2D uTex;   // binding0 = UBO

layout(location = 0) in vec2 vUV;

layout(location = 0) out vec4 FragColor;

void main()
{
    FragColor = texture(uTex, vUV);
}
