#version 450
// Vulkan 变体：贴图模型 片元着色器（与 GL 版 frag_model 语义一致，PBR Cook-Torrance）
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
    vec4 materialParams;   // PBR：x=metallic y=roughness z=has_normal w=ao
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

const float PI = 3.14159265359;

float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    return a2 / (PI * denom * denom);
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return NdotV / (NdotV * (1.0 - k) + k);
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    return GeometrySchlickGGX(max(dot(N, V), 0.0), roughness)
         * GeometrySchlickGGX(max(dot(N, L), 0.0), roughness);
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main()
{
    if (fs_in.pClipDistance < 0.0) discard;   // D7：剖切

    vec3 albedo = texture(texture_diffuse0, fs_in.pTexCoord).rgb * fd.materialDiffuse.xyz;
    float metallic  = fd.materialParams.x;
    float roughness = clamp(fd.materialParams.y, 0.04, 1.0);
    float ao        = fd.materialParams.w;
    bool has_normal_tex = fd.materialParams.z > 0.5;
    bool isOpen = fd.lightPosition_isOpen.w > 0.5;

    vec3 N = normalize(fs_in.pNormal);
    if (has_normal_tex) {
        N = texture(texture_normal0, fs_in.pTexCoord).xyz;
        N = normalize(N * 2.0 - 1.0);
        N = normalize(fs_in.pMatNormal * N);
    }

    vec3 V = normalize(fd.cameraPos.xyz - fs_in.pFragPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    vec3 result = vec3(0.0);

    if (isOpen) {
        vec3 L = normalize(fd.lightPosition_isOpen.xyz - fs_in.pFragPos);
        float cosv = dot(-L, normalize(fd.lightDirection_cosTheta.xyz));

        if (cosv > fd.lightDirection_cosTheta.w) {
            vec3 H = normalize(V + L);
            vec3 radiance = fd.lightDiffuse.xyz;

            float NDF = DistributionGGX(N, H, roughness);
            float G   = GeometrySmith(N, V, L, roughness);
            vec3  F   = fresnelSchlick(max(dot(H, V), 0.0), F0);

            vec3 numerator = NDF * G * F;
            float denominator = 4.0 * max(dot(N, V), 0.0) * max(dot(N, L), 0.0) + 0.0001;
            vec3 specular = numerator / denominator;

            vec3 kS = F;
            vec3 kD = (1.0 - kS) * (1.0 - metallic);
            float NdotL = max(dot(N, L), 0.0);
            result += (kD * albedo / PI + specular) * radiance * NdotL;
        }
    }

    result += fd.lightAmbient.xyz * albedo * ao;

    // V3-19：阴影（3x3 PCF，Reversed-Z）
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
                        litAcc += (proj.z >= d - bias) ? 1.0 : 0.0;
                    }
                }
                float lit = litAcc / 9.0;
                result *= mix(0.45, 1.0, lit);
            }
        }
    }

    // D8：选中高亮混合
    result = mix(result, pc.uHighlight.rgb, pc.uHighlight.a);
    result = mix(result, pc.uHover.rgb, pc.uHover.a);

    FragColor = vec4(result, fd.misc.x);
}
