// RenderPass
#pragma once 

#include "RenderPass.h"

class CPickPass : public IRenderPass {
public:
    CPickPass(std::shared_ptr<RenderContext> content);
	virtual void draw(Scene* pScene) override;
private:
    GLuint _frameBufferID = 0;
};
