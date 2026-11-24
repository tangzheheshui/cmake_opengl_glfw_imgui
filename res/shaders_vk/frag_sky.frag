#version 450
// Vulkan 变体：天空盒 片元着色器（binding4 = 槽 3 cubemap）
layout(set = 4, binding = 0) uniform samplerCube uTex;   // V3-2：槽 3 = set4

layout(location = 0) in vec3 pCoord;

layout(location = 0) out vec4 FragColor;

void main()
{
    FragColor = texture(uTex, pCoord);
}
