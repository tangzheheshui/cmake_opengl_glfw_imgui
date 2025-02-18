// RenderPass
#pragma once 

#include "RenderPass.h"

class CSkyPass : public IRenderPass {
public:
	virtual void draw(Scene* pScene) override;
};
