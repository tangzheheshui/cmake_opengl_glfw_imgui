// IRenderPass
#pragma once 

#include "IRenderPass.h"

class CShadowPass : public IRenderPass {
public:
    CShadowPass(std::shared_ptr<RenderContext> content);
    CShadowPass() {}
	virtual void draw(Scene* pScene) override;
private:
    GLuint _depthMapFBO = 0;
};
