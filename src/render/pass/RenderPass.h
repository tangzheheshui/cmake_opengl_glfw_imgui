// RenderPass

#pragma once 

#include <glad/glad.h>
#include <memory>

struct RenderContext {
    int screenWidth{};
    int screenHeight{};
};

class Scene;
class IRenderPass {
public:
    IRenderPass(std::shared_ptr<RenderContext> content) { mRenderContent = content; }
	virtual void draw(Scene* pScene) = 0;
protected:
    std::shared_ptr<RenderContext> mRenderContent;
};
