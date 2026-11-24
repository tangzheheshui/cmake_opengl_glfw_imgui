#version 450
// Vulkan 变体：SDF 文字 片元着色器。
// 注意设备描述符布局固定：binding0 = 绘制级 UBO（文字不消费），采样器从 1 起
// ——图集绑在 binding1（bindTexture 槽 0 的固定位置）
layout(set = 0, binding = 1) uniform sampler2D uAtlas;

layout(push_constant) uniform PC {
    mat4 uMatrixMVP;   // 占位（顶点消费，保持布局一致）
    vec4 uColor;       // rgb 线性色 + a 全局透明度
} pc;

layout(location = 0) in vec2 vTexCoord;
layout(location = 0) out vec4 FragColor;

void main()
{
    float d = texture(uAtlas, vTexCoord).r;
    float w = fwidth(d);
    float a = smoothstep(0.5 - w, 0.5 + w, d);
    if (a <= 0.003) discard;
    FragColor = vec4(pc.uColor.rgb, a * pc.uColor.a);
}
