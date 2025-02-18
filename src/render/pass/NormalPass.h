// pass虚基类
#pragma once 

#include "RenderPass.h"

class CNormalPass : public IRenderPass {
public:
	virtual void draw(Scene* pScene) override;
};