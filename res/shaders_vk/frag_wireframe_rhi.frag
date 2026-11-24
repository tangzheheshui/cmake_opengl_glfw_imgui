#version 450
// Vulkan 变体：线框片元（纯色直出 + 剖切；uColor 经 PC offset 80）
layout(push_constant) uniform PC {
    mat4 uViewProj;    // 占位（顶点消费）
    vec4 uClipPlane;   // 占位（顶点消费）
    vec4 uColor;       // offset 80
} pc;

layout(location = 0) in float vClipDistance;
layout(location = 0) out vec4 FragColor;

void main()
{
    if (vClipDistance < 0.0) discard;   // D7：剖切
    FragColor = vec4(pc.uColor.rgb, pc.uColor.a);
}
