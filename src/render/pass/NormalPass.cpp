#include "NormalPass.h"
#include "../scene.h"
#include "../object/ImageRectangle.h"
#include "../object/Line.h"
#include "../object/Sky.h"
#include "../model/model.h"

CNormalPass::CNormalPass(std::shared_ptr<RenderContext> content) 
: IRenderPass(content) {
    
}

void CNormalPass::draw(Scene* pScene) {
    if (!pScene) {
        return;
    }
    
    glViewport(0, 0, mRenderContent->screenWidth, mRenderContent->screenHeight);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // 矩阵
    auto mpMatrix = pScene->GetActiveCamera()->GetVPMatrix();
    
    for (auto obj : pScene->GetObjs()) {
        obj->draw(mpMatrix);
    }
}
