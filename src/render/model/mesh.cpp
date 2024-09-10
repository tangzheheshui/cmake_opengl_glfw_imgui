//
//  mesh.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/3.
//

#include "mesh.hpp"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "shader.hpp"
#include "image.hpp"
#include "camera.hpp"

Mesh::Mesh(std::shared_ptr<MeshData> meshData, std::shared_ptr<Materail> matData) {
    if (!meshData) {
        return;
    }
    if (matData->textures.empty()) {
        m_pass = std::make_shared<PassColor>(meshData, matData);
    } else {
        m_pass = std::make_shared<PassTexture>(meshData, matData);
    }
}

bool Mesh::Draw(const std::vector<Matrix> &matModel, uint32_t flags, int numViewpoit) {
    if (m_pass) {
         m_pass->Draw(matModel, flags, numViewpoit);
    }
    return true;
}

