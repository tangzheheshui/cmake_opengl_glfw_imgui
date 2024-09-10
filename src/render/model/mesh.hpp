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
#include "PassColor.hpp"
#include "PassTexture.hpp"
#include "glm/glm.hpp"
#include "model_data.h"

class Mesh {
public:
    Mesh(std::shared_ptr<MeshData> meshData, std::shared_ptr<Materail> matData);
    bool Draw(const std::vector<Matrix> &matModel, uint32_t flags, int numViewpoit = 1);
    void setBoneMat(std::shared_ptr<std::vector<glm::mat4>> mat) { m_pass->m_matBone = mat; }
private:        
    unsigned int _texture;
    std::shared_ptr<RenderPass> m_pass;
}; 

#endif /* mesh_hpp */
