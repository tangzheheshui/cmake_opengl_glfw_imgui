// RenderPass
#pragma once 

#include "RenderPass.h"

class CShadowPass : public IRenderPass {
public:
    CShadowPass(std::shared_ptr<RenderContext> content);
	virtual void draw(Scene* pScene) override;
private:
    GLuint _depthMapFBO = 0;
};
