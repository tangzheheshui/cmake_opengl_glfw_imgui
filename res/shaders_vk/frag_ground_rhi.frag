#version 450
// V3-20：阴影接收面片元：棋盘格底色 + 3x3 PCF 阴影
layout(set = 5, binding = 0) uniform sampler2D uShadowMap;

layout(push_constant) uniform PC {
    mat4 uViewProj;
    mat4 uLightMat;
} pc;

layout(location = 0) in vec3 vWorld;
layout(location = 0) out vec4 FragColor;

void main()
{
    vec3 base = vec3(0.55);
    float checker = mod(floor(vWorld.x * 0.5) + floor(vWorld.z * 0.5), 2.0);
    base *= mix(0.75, 1.0, checker);

    float shadow = 1.0;
    vec2 uv = vec2(0.5);
    vec4 ls = pc.uLightMat * vec4(vWorld, 1.0);
    if (ls.w > 0.0) {
        vec3 proj = ls.xyz / ls.w;
        uv = vec2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
        if (uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0
            && proj.z > 0.0 && proj.z < 1.0) {
            vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
            float lit = 0.0;
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    float d = texture(uShadowMap, uv + vec2(dx, dy) * texel).r;
                    lit += (proj.z >= d - 0.0015) ? 1.0 : 0.0;   // Reversed-Z：越大越近
                }
            }
            shadow = mix(0.35, 1.0, lit / 9.0);
        }
    }
    FragColor = vec4(base * shadow, 1.0);
}
