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
    gl_Position = pos.xyww;
}
