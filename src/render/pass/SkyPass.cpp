#include "SkyPass.h"
#include "scene/scene.h"
#include "object/Sky.h"
#include "scene/camera.h"

CSkyPass::CSkyPass(std::shared_ptr<RenderContext> content) 
: IRenderPass(content) {
    
}

void CSkyPass::draw(Scene* pScene) {
    if (!pScene) {
        return;
    }
    
    glViewport(0, 0, mRenderContent->screenWidth, mRenderContent->screenHeight);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0.2f, 0.3f, 0.3f, .0f);

    if (auto sky = pScene->GetSkyBox()) {
        auto matV = pScene->GetActiveCamera()->GetViewMatrix();
        auto matP = pScene->GetActiveCamera()->GetProjectMatrix();
        sky->draw(matV, matP);
    }
}
