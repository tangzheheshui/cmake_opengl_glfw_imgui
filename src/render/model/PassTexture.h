//
//  PassTexture.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/22.
//

#ifndef PassTexture_hpp
#define PassTexture_hpp

#include "mesh_pass.h"

class PassTexture : public MeshPass {
public:
    using MeshPass::MeshPass;
    
protected:
    bool isTextured() const override { return true; }

private:
    void setup(const std::vector<Matrix> &matModel, uint32_t flags) override;
    Shader* getShader(uint32_t flags) override;
};

#endif /* PassTexture_hpp */
