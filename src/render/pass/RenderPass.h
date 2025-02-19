// RenderPass

#pragma once 

#include <glad/glad.h>
#include <memory>

class RenderContext {
public:
    int screenWidth{};
    int screenHeight{};
};

class Scene;
class IRenderPass {
public:
    IRenderPass() {}
    IRenderPass(std::shared_ptr<RenderContext> content) { mRenderContent = content; }
	virtual void draw(Scene* pScene) = 0;
protected:
    std::shared_ptr<RenderContext> mRenderContent;
};
