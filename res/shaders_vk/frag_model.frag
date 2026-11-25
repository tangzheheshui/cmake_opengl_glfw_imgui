#version 450
// Vulkan 变体：贴图模型 片元着色器（与 GL 版 frag_model 语义一致）
layout(set = 0, binding = 0) uniform FrameData {
    vec4 lightPosition_isOpen;
    vec4 lightDirection_cosTheta;
    vec4 lightAmbient;
    vec4 lightDiffuse;
    vec4 lightSpecular;
    vec4 cameraPos;
    vec4 materialAmbient;
    vec4 materialDiffuse;
    vec4 materialSpecular;
    vec4 materialParams;   // x=shininess y=strength z=has_normal w=has_specular
    vec4 misc;             // D3：x=opacity；V3-19：y=阴影开关
    mat4 lightVP;          // V3-19：光空间矩阵（已做 Reversed-Z 修正；布局同步 240B）
} fd;

layout(set = 1, binding = 0) uniform sampler2D texture_diffuse0;
layout(set = 2, binding = 0) uniform sampler2D texture_specular0;
layout(set = 3, binding = 0) uniform sampler2D texture_normal0;
// V3-19：阴影贴图固定在槽 4（set5）；未绑定时引擎填白纹理 + misc.y=0 不采样
layout(set = 5, binding = 0) uniform sampler2D uShadowMap;

// D8：高亮经 push constant offset 80（布局与 vert_model 的 PC 块一致：
// mat4@0 + vec4@64 + vec4@80；前两项此着色器不消费，仅占位对齐）
layout(push_constant) uniform PC {
    mat4 uViewProj;
    vec4 uClipPlane;
    vec4 uHighlight;   // offset 80：rgb 颜色 + a 强度
    vec4 uHover;       // offset 96：悬停提示色
} pc;

layout(location = 0) in VS_OUT
{
    layout(location = 0) vec3 pFragPos;
    layout(location = 1) vec3 pNormal;
    layout(location = 2) vec2 pTexCoord;
    layout(location = 3) mat3 pMatNormal;
    layout(location = 6) float pClipDistance;
} fs_in;

layout(location = 0) out vec4 FragColor;

void main()
{
    if (fs_in.pClipDistance < 0.0) discard;   // D7：剖切
    vec3 color = texture(texture_diffuse0, fs_in.pTexCoord).xyz;
    vec3 result = color;

    bool isOpen = fd.lightPosition_isOpen.w > 0.5;
    bool has_normal_tex   = fd.materialParams.z > 0.5;
    bool has_specular_tex = fd.materialParams.w > 0.5;

    if (isOpen) {
        vec3 normal = normalize(fs_in.pNormal);
        vec3 dirLight = normalize(fd.lightPosition_isOpen.xyz - fs_in.pFragPos);
        float cosv = dot(-dirLight, normalize(fd.lightDirection_cosTheta.xyz));

        if (has_normal_tex) {
            normal = texture(texture_normal0, fs_in.pTexCoord).xyz;
            normal = normalize(normal * 2.0 - 1.0);
            normal = normalize(fs_in.pMatNormal * normal);
        }

        // ambient
        result = fd.lightAmbient.xyz * color;

        if (cosv > fd.lightDirection_cosTheta.w) {
            // diffuse
            float diff = max(dot(dirLight, normal), 0.0);
            result += fd.lightDiffuse.xyz * color * diff;

            // specular
            vec3 viewDir = normalize(fd.cameraPos.xyz - fs_in.pFragPos);
            vec3 halfwayDir = normalize(dirLight + viewDir);
            float spec = pow(max(dot(normal, halfwayDir), 0.0), fd.materialParams.x);
            vec3 specular = fd.lightSpecular.xyz * spec * fd.materialSpecular.xyz;
            if (has_specular_tex) {
                specular = fd.lightSpecular.xyz * spec * vec3(texture(texture_specular0, fs_in.pTexCoord));
            }
            result += specular;
        }
    }

    // V3-19：阴影（3x3 PCF）。阴影贴图按 Reversed-Z 写入（越大越近），
    // 采样坐标做 Y 翻转（阴影 Pass 用负高度视口，纹理行序与 NDC 相反）
    if (fd.misc.y > 0.5 && isOpen) {
        vec4 ls = fd.lightVP * vec4(fs_in.pFragPos, 1.0);
        if (ls.w > 0.0) {
            vec3 proj = ls.xyz / ls.w;
            vec2 uv = vec2(proj.x * 0.5 + 0.5, 0.5 - proj.y * 0.5);
            if (uv.x > 0.0 && uv.x < 1.0 && uv.y > 0.0 && uv.y < 1.0
                && proj.z > 0.0 && proj.z < 1.0) {
                vec2 texel = 1.0 / vec2(textureSize(uShadowMap, 0));
                float bias = 0.0015;
                float litAcc = 0.0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        float d = texture(uShadowMap, uv + vec2(dx, dy) * texel).r;
                        litAcc += (proj.z >= d - bias) ? 1.0 : 0.0;   // Reversed-Z：越大越近
                    }
                }
                float lit = litAcc / 9.0;
                result *= mix(0.45, 1.0, lit);   // 阴影区压暗到 45%
            }
        }
    }

    // D8：选中高亮混合（uHighlight.a = 0 时无效果）
    result = mix(result, pc.uHighlight.rgb, pc.uHighlight.a);
    result = mix(result, pc.uHover.rgb, pc.uHover.a);   // 悬停提示（选中优先级更高）

    FragColor = vec4(result, fd.misc.x);   // D3：材质不透明度
}
