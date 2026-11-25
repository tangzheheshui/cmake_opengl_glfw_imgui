//
//  mesh.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/3.
//

#ifndef mesh_hpp
#define mesh_hpp

#include <string>
#include <vector>

#include "assimp/Importer.hpp"
#include "assimp/scene.h"
#include "assimp/postprocess.h" 
#include "PassColor.h"
#include "PassTexture.h"
#include "glm/glm.hpp"
#include "model_data.h"
#include "rhi/RhiTypes.h"

namespace rhi { class IRHICommandList; }

class Mesh {
public:
    Mesh(std::shared_ptr<MeshData> meshData, std::shared_ptr<Material> matData);
    bool Draw(const std::vector<Matrix> &matModel, const Matrix &mtx, uint32_t flags, int numViewpoit = 1, float opacity = 1.0f);
    bool DrawPick(const std::vector<Matrix> &matModel, const Matrix &mtx, uint16_t pickID);
    void setBoneMat(std::shared_ptr<std::vector<glm::mat4>> mat) { m_pass->m_matBone = mat; }
    // 几何共享（合批前置）：模型解析缓存取回 MeshData 用
    std::shared_ptr<MeshData> meshData() const { return m_pass ? m_pass->m_mesh_data : nullptr; }
    // D2: RHI 拾取绘制经 MeshPass（pick 管线由调用方传入）
    bool DrawPickRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle pickPipeline,
                     const Matrix& viewProj, const std::vector<Matrix>& matModel, uint16_t pickID) {
        return m_pass ? m_pass->DrawPickRHI(cmd, pickPipeline, viewProj, matModel, pickID) : false;
    }
    // V3-18：阴影 Pass 绘制（经 MeshPass 的深度专用管线）
    bool DrawShadowRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle shadowPipeline,
                       const Matrix& lightVP, const std::vector<Matrix>& matModel) {
        return m_pass ? m_pass->DrawShadowRHI(cmd, shadowPipeline, lightVP, matModel) : false;
    }
private:
    unsigned int _texture;
    std::shared_ptr<MeshPass> m_pass;
};

#endif /* mesh_hpp */
