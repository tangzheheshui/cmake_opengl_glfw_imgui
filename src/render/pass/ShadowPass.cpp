#include "ShadowPass.h"
#include "../scene.h"
#include "../object/ImageRectangle.h"
#include "../object/Line.h"
#include "../object/Sky.h"
#include "../model/model.h"

const GLuint SHADOW_WIDTH = 1024, SHADOW_HEIGHT = 1024;

CShadowPass::CShadowPass() {  
    glGenFramebuffers(1, &_depthMapFBO);
}

void CShadowPass::draw(Scene* pScene) {
    glBindFramebuffer(GL_FRAMEBUFFER, _depthMapFBO);
    auto _depthTexture = TextureMng::getInstance().getTexture(STR_DEPTH_TEXTURE);
    assert(_depthTexture);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _depthTexture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // bind
    glViewport(0, 0, SHADOW_WIDTH, SHADOW_HEIGHT);
    glBindFramebuffer(GL_FRAMEBUFFER, _depthMapFBO);
    glClear(GL_DEPTH_BUFFER_BIT);
    
    // draw
    for (auto obj : pScene->GetObjs()) {
        obj->drawShadow();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
