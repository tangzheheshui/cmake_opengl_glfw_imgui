//
//  pass.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/22.
//

#ifndef pass_hpp
#define pass_hpp

#include <vector>
#include <map>
#include "rhi/RhiTypes.h"
#include "glm/glm.hpp"
#include <glad/glad.h>
#include "camera/camera.h"
#include "shader.h"
#include "model_data.h"
#include "math/matrix.h"

namespace rhi { class IRHIDevice; }
namespace rhi { class IRHICommandList; }

class MeshPass {
public:
    MeshPass(std::shared_ptr<MeshData> meshData, std::shared_ptr<Material> matData);
    virtual ~MeshPass() {
        if (_VAO > 0) {
            glDeleteVertexArrays(1, &_VAO);
        }
        
        if (_VBO > 0) {
            glDeleteBuffers(1, &_VBO);
        }
        
        if (_EBO > 0) {
            glDeleteBuffers(1, &_EBO);
        }
    }
    
    bool Draw(const std::vector<Matrix> &matModel, const Matrix &mtx, uint32_t flags, int numViewpoit);
    bool DrawPick(const std::vector<Matrix> &matModel, const Matrix &mtx, uint16_t pickID);
    // M4.2: RHI 绘制路径（Vulkan 必走；GL 可经 modelUseRHI 开关切换对比）
    bool DrawRHI(const std::vector<Matrix> &matModel, const Matrix &mtx, uint32_t flags);
    // D2: RHI 拾取绘制（pick 管线 + ID 色直出；调用方管理 RT/pass）
    bool DrawPickRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle pickPipeline,
                     const Matrix& viewProj, const std::vector<Matrix>& matModel,
                     uint16_t pickID);
protected:
    virtual Shader* getShader(uint32_t flags) { return nullptr; }
    virtual bool isTextured() const { return false; }   // PassTexture 覆写为 true
    virtual void setup(const std::vector<Matrix> &matModel, uint32_t flags) {}
    void setup(const std::vector<Matrix> &matModel, bool hasTexture, bool hasAni);
    void ensureRHIResources(rhi::IRHIDevice* device);   // 惰性创建 RHI 资源
public:
    std::shared_ptr<Material> m_material;
    std::shared_ptr<MeshData> m_mesh_data;
    unsigned int _VAO = 0;
    unsigned int _VBO = 0;
    unsigned int _EBO = 0;
    std::shared_ptr<std::vector<glm::mat4>> m_matBone;

    // ---- RHI 资源（M4.2 迁移）----
    rhi::BufferHandle m_rhiVB;
    rhi::BufferHandle m_rhiInstanceVB;
    rhi::BufferHandle m_rhiIB;
    rhi::PipelineHandle m_rhiPipeline;
    rhi::UniformBlockHandle m_rhiUniformBlock;
    bool m_rhiBuilt = false;
    std::map<std::string, rhi::TextureHandle> m_rhiTexCache;   // 材质纹理缓存（文件名 → RHI 纹理）
};

#endif /* pass_hpp */
