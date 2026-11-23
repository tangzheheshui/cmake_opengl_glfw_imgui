// IRenderPass
#pragma once 

#include "IRenderPass.h"

class CSkyPass : public IRenderPass {
public:
    CSkyPass(std::shared_ptr<RenderContext> content);
	virtual void draw(Scene* pScene) override;
};
