//
//  DrawUniforms.h
//  绘制级 uniform 数据块 —— 引擎固定布局（唯一约定点）
//
//  生产方：MeshPass/PassTexture 等迁移对象（一次 setDrawUniforms 提交）
//  消费方：GL 后端按字段拆解为 glUniform*（名字见各字段注释）；
//          Vulkan 后端整块写入共享 UBO（binding 0），VK 着色器按同布局声明
//          （见 res/shaders_vk/frag_model.frag 的 FrameData 块）
//
#ifndef DrawUniforms_h
#define DrawUniforms_h

#include <glm/glm.hpp>

namespace engine {

struct DrawUniforms {
    glm::vec4 lightPosition_isOpen;      // uLight.position(xyz) / uLight.is_open(w: 1=开)
    glm::vec4 lightDirection_cosTheta;   // uLight.direction(xyz) / uLight.cosTheta(w)
    glm::vec4 lightAmbient;              // uLight.ambient
    glm::vec4 lightDiffuse;              // uLight.diffuse
    glm::vec4 lightSpecular;             // uLight.specular
    glm::vec4 cameraPos;                 // uCameraPos
    glm::vec4 materialAmbient;           // uMaterail.ambient
    glm::vec4 materialDiffuse;           // uMaterail.diffuse
    glm::vec4 materialSpecular;          // uMaterail.specular
    glm::vec4 materialParams;            // PBR：x=metallic y=roughness z=has_normal_tex w=ao
    glm::vec4 misc;                      // D3：x=opacity；V3-19：y=阴影开关（1=采样阴影贴图）
    // V3-19：光空间矩阵（已做 Reversed-Z 修正）。阴影贴图按它写入，着色器按同一约定比较
    glm::mat4 lightVP;
};
static_assert(sizeof(DrawUniforms) == 240, "DrawUniforms layout must stay 240 bytes");

} // namespace engine

#endif /* DrawUniforms_h */
