//
//  PassColor.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/22.
//

#ifndef PassColor_hpp
#define PassColor_hpp

#include "mesh_pass.h"

class PassColor : public MeshPass {
public:
    using MeshPass::MeshPass;

private:
    virtual Shader* getShader(uint32_t flags) override;
    virtual void setup(const std::vector<Matrix> &matModel, uint32_t flags) override;
private:
    
};

#endif /* PassColor_hpp */
