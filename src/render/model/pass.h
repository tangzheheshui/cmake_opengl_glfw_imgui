//
//  pass.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/22.
//

#ifndef pass_hpp
#define pass_hpp

#include <vector>
#include "glm/glm.hpp"
#include <glad/glad.h>
#include "camera/camera_old.h"
#include "shader.h"
#include "model_data.h"
#include "math/matrix.h"

class RenderPass {
public:
    RenderPass(std::shared_ptr<MeshData> meshData, std::shared_ptr<Materail> matData);
    virtual ~RenderPass() {
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
    
    bool Draw(const std::vector<Matrix> &matModel, CameraPtr camera, uint32_t flags, int numViewpoit);
    bool DrawPick(const std::vector<Matrix> &matModel, const Matrix &mtx, uint16_t pickID);
protected:
    virtual Shader* getShader(uint32_t flags) { return nullptr; }
    virtual void setup(const std::vector<Matrix> &matModel, uint32_t flags) {}
    void setup(const std::vector<Matrix> &matModel, bool hasTexture, bool hasAni);
public:
    std::shared_ptr<Materail> m_materail;
    std::shared_ptr<MeshData> m_mesh_data;
    unsigned int _VAO = 0;
    unsigned int _VBO = 0;
    unsigned int _EBO = 0;
    std::shared_ptr<std::vector<glm::mat4>> m_matBone;
};

#endif /* pass_hpp */
