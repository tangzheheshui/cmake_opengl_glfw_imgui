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
} fd;

layout(set = 0, binding = 1) uniform sampler2D texture_diffuse0;
layout(set = 0, binding = 2) uniform sampler2D texture_specular0;
layout(set = 0, binding = 3) uniform sampler2D texture_normal0;

layout(location = 0) in VS_OUT
{
    layout(location = 0) vec3 pFragPos;
    layout(location = 1) vec3 pNormal;
    layout(location = 2) vec2 pTexCoord;
    layout(location = 3) mat3 pMatNormal;
} fs_in;

layout(location = 0) out vec4 FragColor;

void main()
{
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

    FragColor = vec4(result, 1.0);
}
