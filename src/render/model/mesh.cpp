//
//  mesh.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/3.
//

#include "mesh.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "shader.h"
#include "image.h"

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

bool Mesh::Draw(const std::vector<Matrix> &matModel, CameraPtr camera, uint32_t flags, int numViewpoit) {
    if (m_pass) {
         m_pass->Draw(matModel, camera, flags, numViewpoit);
    }
    return true;
}

bool Mesh::DrawPick(const std::vector<Matrix> &matModel, const Matrix &mtx, uint16_t pickID) {
    if (m_pass) {
         m_pass->DrawPick(matModel, mtx, pickID);
    }
    return true;
}

