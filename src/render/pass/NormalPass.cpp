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
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // 矩阵
    auto mpMatrix = pScene->GetActiveCamera()->GetVPMatrix();
    
    for (auto obj : pScene->GetObjs()) {
        obj->draw(mpMatrix);
    }
}
