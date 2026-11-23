#version 450
// Vulkan 变体：D1 宽线（RHI 展开路径）顶点着色器。
// 与 GL 版 res/shaders/vert_line_rhi 语义逐行一致，仅 uniform 来源不同
// （GL 按名 uniform → VK push constant 固定布局：uMatrixMVP@0 / uColor@64）。
// 注意 B3a：VK 端用负高度 viewport 实现 GL 约定的 y 向上；本实现全部在
// NDC 空间展开（像素方向由端点 NDC 推出），viewport 翻转与方向翻转相互
// 抵消，故无需按后端区分偏移符号，双端屏幕结果一致。
layout(location = 0) in vec3 aPosA;
layout(location = 1) in vec3 aPosB;
layout(location = 2) in vec4 aLine;    // (端别 0=A 端/1=B 端, 侧别 ±1, 宽度像素, 保留)
layout(location = 3) in vec2 aScreen;  // (2/viewportW, 2/viewportH)

layout(push_constant) uniform PC {
    mat4 uMatrixMVP;
    vec4 uColor;
} pc;

void main()
{
    vec4 clipA = pc.uMatrixMVP * vec4(aPosA, 1.0);
    vec4 clipB = pc.uMatrixMVP * vec4(aPosB, 1.0);
    // 任一端点在相机后方（w<=0）：透视除法翻转导致展开方向失真，整段退化丢弃
    if (clipA.w <= 0.0 || clipB.w <= 0.0) {
        gl_Position = vec4(2.0, 2.0, 2.0, 1.0);   // 裁剪体外，不可见
        return;
    }
    vec2 ndcA = clipA.xy / clipA.w;
    vec2 ndcB = clipB.xy / clipB.w;
    vec2 dPx = (ndcB - ndcA) / aScreen;          // 像素空间方向向量
    float len = length(dPx);
    dPx = len > 1e-6 ? dPx / len : vec2(1.0, 0.0);
    vec2 offsetPx = vec2(-dPx.y, dPx.x) * (aLine.y * aLine.z * 0.5);
    vec4 clip = mix(clipA, clipB, aLine.x);
    clip.xy += (offsetPx * aScreen) * clip.w;
    gl_Position = clip;
}
