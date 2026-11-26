//
//  mesh.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/3.
//

#include "mesh.h"
#include "mesh_pass.h"

Mesh::Mesh(std::shared_ptr<MeshData> meshData, std::shared_ptr<Material> matData) {
    if (!meshData) {
        return;
    }
    // 去后端化：着色器选择（贴图/纯色）由 MeshPass 按材质自行决定
    m_pass = std::make_shared<MeshPass>(meshData, matData);
}

bool Mesh::Draw(rhi::IRHICommandList* cmd, const std::vector<Matrix> &matModel, const Matrix &mtx, uint32_t flags, int numViewpoit, float opacity) {
    if (m_pass) {
         m_pass->Draw(cmd, matModel, mtx, flags, numViewpoit, opacity);
    }
    return true;
}

void Mesh::setBoneMat(std::shared_ptr<std::vector<glm::mat4>> mat) {
    if (m_pass) m_pass->m_matBone = mat;
}

std::shared_ptr<MeshData> Mesh::meshData() const {
    return m_pass ? m_pass->m_mesh_data : nullptr;
}

std::shared_ptr<Material> Mesh::material() const {
    return m_pass ? m_pass->m_material : nullptr;
}

bool Mesh::DrawPickRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle pickPipeline,
                       const Matrix& viewProj, const std::vector<Matrix>& matModel, uint16_t pickID) {
    return m_pass ? m_pass->DrawPickRHI(cmd, pickPipeline, viewProj, matModel, pickID) : false;
}

bool Mesh::DrawShadowRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle shadowPipeline,
                         const Matrix& lightVP, const std::vector<Matrix>& matModel) {
    return m_pass ? m_pass->DrawShadowRHI(cmd, shadowPipeline, lightVP, matModel) : false;
}

