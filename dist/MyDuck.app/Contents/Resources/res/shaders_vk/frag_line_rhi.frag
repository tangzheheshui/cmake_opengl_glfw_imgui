#version 450
// Vulkan 变体：D1 收尾——边缘抗锯齿 + 圆头端点（与 GL 版 frag_line_rhi 语义一致）
layout(push_constant) uniform PC {
    mat4 uMatrixMVPPad;
    vec4 uColor;
} pc;

layout(location = 0) in vec4 vLine;
layout(location = 0) out vec4 FragColor;

void main()
{
    const float halfW = vLine.z;
    const float along = vLine.y;
    const bool  round = vLine.w > 0.0;

    float r = abs(vLine.x);                                   // 默认：侧向距离
    if (round) {
        if (along < 0.0) {
            r = length(vec2(along, vLine.x));                 // A 端圆头
        } else if (along > vLine.w) {
            r = length(vec2(along - vLine.w, vLine.x));       // B 端圆头
        }
    }

    const float alpha = 1.0 - smoothstep(halfW - 1.0, halfW, r);
    if (alpha <= 0.003) discard;
    FragColor = vec4(pc.uColor.rgb, alpha);
}
