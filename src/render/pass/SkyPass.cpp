#include "SkyPass.h"
#include "../scene.h"
#include "../object/Sky.h"
#include "../camera/camera_old.h"

CSkyPass::CSkyPass(std::shared_ptr<RenderContext> content) 
: IRenderPass(content) {
    
}

void CSkyPass::draw(Scene* pScene) {
    if (!pScene) {
        return;
    }
    
    glViewport(0, 0, mRenderContent->screenWidth, mRenderContent->screenHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    if (auto sky = pScene->GetSkyBox()) {
        auto matV = pScene->GetActiveCamera()->GetViewMatrix();
        auto matP = pScene->GetActiveCamera()->GetProjectMatrix();
        sky->draw(matV, matP);
    }
}
