#version 450
// Vulkan 变体：RHI 拾取 顶点着色器（D2）
// 只消费位置 + 实例矩阵；法线/UV 在管线顶点布局中存在但着色器不消费。
// push constant 布局与全部管线一致（uViewProj@0 / uPickColor@64）
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 iRow0;
layout(location = 4) in vec4 iRow1;
layout(location = 5) in vec4 iRow2;
layout(location = 6) in vec4 iRow3;

layout(push_constant) uniform PC {
    mat4 uViewProj;    // offset 0
    vec4 uPickColor;   // offset 64：对象 ID 编码色（CPU 侧编码，ID 渲染直出）
} pc;

void main()
{
    mat4 aInstanceMatrix = mat4(iRow0, iRow1, iRow2, iRow3);
    gl_Position = pc.uViewProj * aInstanceMatrix * vec4(aPos, 1.0);
}
