// RenderPass
#pragma once 

#include "RenderPass.h"

class CSkyPass : public IRenderPass {
public:
    CSkyPass(std::shared_ptr<RenderContext> content);
	virtual void draw(Scene* pScene) override;
};
