#version 450
// Vulkan 变体：贴图模型 顶点着色器（实例化矩阵 + FrameData UBO）
// 与 GL 版 vert_model 语义一致
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;
// 实例矩阵（4 个连续 location，binding1 = 实例率）
layout(location = 3) in vec4 iRow0;
layout(location = 4) in vec4 iRow1;
layout(location = 5) in vec4 iRow2;
layout(location = 6) in vec4 iRow3;

// 绘制级 uniform UBO（布局约定见 src/render/DrawUniforms.h）
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
    vec4 materialParams;
} fd;

layout(push_constant) uniform PC {
    mat4 uViewProj;
    vec4 uClipPlane;   // D7：xyz 法线，w 距离；offset 64
} pc;

out VS_OUT
{
    layout(location = 0) vec3 pFragPos;
    layout(location = 1) vec3 pNormal;
    layout(location = 2) vec2 pTexCoord;
    layout(location = 3) mat3 pMatNormal;   // 占 location 3,4,5
    layout(location = 6) float pClipDistance;
} vs_out;

void main()
{
    mat4 aInstanceMatrix = mat4(iRow0, iRow1, iRow2, iRow3);
    vs_out.pFragPos = (aInstanceMatrix * vec4(aPos, 1.0)).xyz;
    vs_out.pClipDistance = dot(vec4(vs_out.pFragPos, 1.0), pc.uClipPlane);
    vs_out.pMatNormal = mat3(transpose(inverse(aInstanceMatrix)));
    vs_out.pNormal = vs_out.pMatNormal * aNormal;
    vs_out.pTexCoord = aTexCoord;
    gl_Position = pc.uViewProj * aInstanceMatrix * vec4(aPos, 1.0);
}
