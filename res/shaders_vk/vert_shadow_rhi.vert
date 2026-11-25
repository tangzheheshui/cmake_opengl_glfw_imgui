#version 450
// V3-18：阴影 Pass 顶点着色器（深度专用，无颜色输出）
// 顶点布局与 vert_model 一致：aPos + 每实例 mat4（location 3..6）
layout(location = 0) in vec3 aPos;
layout(location = 3) in vec4 iRow0;
layout(location = 4) in vec4 iRow1;
layout(location = 5) in vec4 iRow2;
layout(location = 6) in vec4 iRow3;

// 光空间矩阵放在 push constant 槽 0（与其它管线同一布局）。
// 名字不含 Proj/MVP：VK 后端的 Reversed-Z 修正是按名判定的，
// 这里传的已经是修正过的矩阵（渲染侧预处理），不能再被改一次。
layout(push_constant) uniform PC {
    mat4 uLightMat;
} pc;

void main()
{
    mat4 inst = mat4(iRow0, iRow1, iRow2, iRow3);
    gl_Position = pc.uLightMat * inst * vec4(aPos, 1.0);
}
