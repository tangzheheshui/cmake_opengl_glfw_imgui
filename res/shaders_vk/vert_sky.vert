#version 450
// Vulkan 变体：天空盒 顶点着色器（与 GL 版 vert_sky 语义一致）
layout(location = 0) in vec3 aPos;

layout(push_constant) uniform PC {
    mat4 uViewMat;   // offset 0
    mat4 uProjMat;   // offset 64
} pc;

layout(location = 0) out vec3 pCoord;

void main()
{
    pCoord = aPos;
    vec4 pos = pc.uProjMat * mat4(mat3(pc.uViewMat)) * vec4(aPos, 1.0);
    // 天空盒固定画在远平面。GL 约定远平面 z_ndc=1（z=w）；本后端是 Reversed-Z，
    // 远平面 = 0，且管线比较算子为 GREATER_OR_EQUAL（sky 深度 0 恒通过背景、
    // 输给任何已写深度的前景）。uProjMat 已在 VkCommandList::setUniform 做过修正。
    gl_Position = vec4(pos.xy, 0.0, pos.w);
}
