// pass虚基类
#pragma once 

#include "IRenderPass.h"

class CNormalPass : public IRenderPass {
public:
    CNormalPass(std::shared_ptr<RenderContext> content);
	virtual void draw(Scene* pScene) override;
};
