// RenderPass

#pragma once 

#include <glad/glad.h>

class Scene;
class IRenderPass {
public:
	virtual void draw(Scene* pScene) = 0;
};
