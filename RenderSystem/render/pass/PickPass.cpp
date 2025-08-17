#include "PickPass.h"
#include "scene/scene.h"
#include "../object/ImageRectangle.h"
#include "../object/Line.h"
#include "../object/Sky.h"
#include "../model/model.h"

CPickPass::CPickPass(std::shared_ptr<RenderContext> content)
: IRenderPass(content) {  
    glGenFramebuffers(1, &_frameBufferID);
}

void CPickPass::draw(Scene* pScene) {
    glBindFramebuffer(GL_FRAMEBUFFER, _frameBufferID);
    auto _texture = TextureMng::getInstance().getTexture(STR_PICK_TEXTURE);
    assert(_texture);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, _texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // bind
    glViewport(0, 0, mRenderContent->screenWidth, mRenderContent->screenHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, _frameBufferID);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // draw
    for (auto obj : pScene->GetObjs()) {
        obj->drawShadow();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
