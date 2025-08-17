//
//  pass.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/22.
//

#include "pass.h"
#include "../scene.h"
#include "../Light.h"
#include "image.h"

RenderPass::RenderPass(std::shared_ptr<MeshData> meshData, std::shared_ptr<Materail> matData) {
    m_mesh_data = meshData;
    m_materail = matData;
}

bool RenderPass::DrawPick(const std::vector<Matrix> &matModel, const Matrix &mtx, uint16_t pickID) {
    auto shader = ShaderCache::GetInstance().GetShader(ShaderType::Pick);   
    if (!shader) {
        return false;
    }
    
    shader->use();
    
    shader->setInt("uPickID", pickID);
    
    setup(matModel, 0);
  
    // 绘制网格
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glBindVertexArray(_VAO);
    glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)m_mesh_data->indices.size(), GL_UNSIGNED_INT, 0,  (int)matModel.size());
    glBindVertexArray(0);
    
    return true;
}

bool RenderPass::Draw(const std::vector<Matrix> &matModel, CameraPtr camera, uint32_t flags, int numViewpoit) {
    bool bDrawShadow = (flags & DrawOption::DRAW_SHADOW);
    auto shader = getShader(flags);
    if (!shader) {
        return false;
    }
    
    shader->use();
    
    bool bDrawPick = (flags & DrawOption::DRAW_PICK);
 
    if (bDrawShadow) {
        auto lightSpaceMatrix = Scene::GetLightVPMatrix();
        shader->setMat4("lightSpaceMatrix", lightSpaceMatrix);
    }
    else if(bDrawPick) {
        // 设置实体的ID
        shader->setInt("uPickID", 0);
    }
    else {
        bool has_normal_tex = false;
        bool has_specular_tex = false;
        
        for (int i = 0; i < m_materail->textures.size(); i++) {
            auto tex = m_materail->textures[i];
            int texID = 0;
            if (!tex.filepath.empty()) {
                texID = TextureMng::getInstance().getTexture(tex.filepath);
                if(tex.name == "texture_diffuse0") {
                    
                }
                else if(tex.name == "texture_specular0") {
                    has_specular_tex = true;
                }
                else if(tex.name == "texture_normal0") {
                    if (!m_mesh_data->tangents.empty()) {
                        has_normal_tex = true;
                    }
                }
                else if(tex.name == "texture_height0") {
                }
            }
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, texID);
            shader->setInt(tex.name, i);
            shader->setBool("uMaterail.has_normal_tex", has_normal_tex);
            shader->setBool("uMaterail.has_specular_tex", has_specular_tex);
        }
        
        // 矩阵
        shader->setMat4("uViewProj", camera->GetVPMatrix());
        
        if (numViewpoit > 1) {
            shader->setMat4Array("uMatrixModel", matModel);
            shader->setInt("uNumViewport", numViewpoit);
        }
        // 灯光
        bool lightOpen = flags&DrawOption::LIGHT_OPEN;
        shader->setBool("uLight.is_open", lightOpen);
        auto light = Light::GlobalLight();
        shader->setFloat4("uLight.position", light.position.x, light.position.y, light.position.z, 1);
        shader->setFloat3("uLight.direction", light.direction.x, light.direction.y, light.direction.z);
        shader->setFloat("uLight.cosTheta", light.cosTheta);
        shader->setFloat3("uLight.ambient", light.ambient.x, light.ambient.y, light.ambient.z);
        shader->setFloat3("uLight.diffuse", light.diffuse.x, light.diffuse.y, light.diffuse.z);
        shader->setFloat3("uLight.specular", light.specular.x, light.specular.y, light.specular.z);
        
        // 材质
        shader->setFloat3("uMaterail.ambient", m_materail->ambient.r, m_materail->ambient.g, m_materail->ambient.b);
        shader->setFloat3("uMaterail.diffuse", m_materail->diffuse.r, m_materail->diffuse.g, m_materail->diffuse.b);
        shader->setFloat3("uMaterail.specular", m_materail->specular.r, m_materail->specular.g, m_materail->specular.b);
        shader->setFloat("uMaterail.shininess", m_materail->shininess);
        shader->setFloat("uMaterail.shininess_strength", m_materail->shininess_strength);
        
        // 相机位置
        auto cam_pos = camera->getPossition();
        shader->setFloat3("uCameraPos", cam_pos.x, cam_pos.y, cam_pos.z);
        
        // boneMat
        if (m_matBone && !m_matBone->empty()) {
            for (int i = 0; i < m_matBone->size(); ++i) {
                auto mat = Matrix::toMatrix(m_matBone->at(i));
                shader->setMat4("uFinalBonesMatrices[" + std::to_string(i) + "]", mat);
            }
        }
    }
    
    setup(matModel, flags);
  
    // 绘制网格
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glBindVertexArray(_VAO);
    glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)m_mesh_data->indices.size(), GL_UNSIGNED_INT, 0,  (int)matModel.size());
    glBindVertexArray(0);
    
    return true;
}

void RenderPass::setup(const std::vector<Matrix> &matModel, bool hasTexture, bool hasAni) {
    if (_VBO == 0) {
        glGenVertexArrays(1, &_VAO);
        glGenBuffers(1, &_VBO);
        glGenBuffers(1, &_EBO);
    }
    
    glBindVertexArray(_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, _VBO);
    
    
    size_t size1 = m_mesh_data->positions.size() * sizeof(glm::vec3);
    size_t size2 = m_mesh_data->normals.size() * sizeof(glm::vec3);
    size_t size3 = m_mesh_data->boneIDs.size() * sizeof(glm::ivec4);
    size_t size4 = m_mesh_data->weights.size() * sizeof(glm::vec4);
    size_t totalsize = size1 + size2;
    if (hasAni) {
        totalsize += (size3 + size4);
    }
    size_t size5 = sizeof(Matrix) * matModel.size();
    totalsize += size5;
    
    glBufferData(GL_ARRAY_BUFFER, totalsize, nullptr , GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, size1, m_mesh_data->positions.data());
    glBufferSubData(GL_ARRAY_BUFFER, size1, size2, m_mesh_data->normals.data());
    
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);
    
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)size1);
    glEnableVertexAttribArray(1);
    
    int bufferIndex = 2;
    if (hasAni) {
        glBufferSubData(GL_ARRAY_BUFFER, size1 + size2, size3, m_mesh_data->boneIDs.data());
        glBufferSubData(GL_ARRAY_BUFFER, size1 + size2 + size3, size4, m_mesh_data->weights.data());
        
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_INT, GL_FALSE, sizeof(glm::ivec4), (void*)(size1 + size2));
        
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), (void*)(size1 + size2 + size3));
        bufferIndex += 2;
    }
    
    glBufferSubData(GL_ARRAY_BUFFER, totalsize - size5, size5, matModel.data());
    glVertexAttribPointer(bufferIndex, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(totalsize - size5));
    glVertexAttribDivisor(bufferIndex, 1);
    glEnableVertexAttribArray(bufferIndex++);
    
    glVertexAttribPointer(bufferIndex, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(totalsize - size5 + sizeof(glm::vec4)));
    glVertexAttribDivisor(bufferIndex, 1);
    glEnableVertexAttribArray(bufferIndex++);
    
    glVertexAttribPointer(bufferIndex, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(totalsize - size5 + 2 * sizeof(glm::vec4)));
    glVertexAttribDivisor(bufferIndex, 1);
    glEnableVertexAttribArray(bufferIndex++);
    
    glVertexAttribPointer(bufferIndex, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(totalsize - size5 + 3 * sizeof(glm::vec4)));
    glVertexAttribDivisor(bufferIndex, 1);
    glEnableVertexAttribArray(bufferIndex++);

    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_mesh_data->indices.size() * sizeof(unsigned int), m_mesh_data->indices.data(), GL_STATIC_DRAW);
    
    glBindVertexArray(0);
}
