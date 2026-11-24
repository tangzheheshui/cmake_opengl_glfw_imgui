#include "SkyPass.h"
#include "../scene.h"
#include "../object/Sky.h"
#include "../camera/camera.h"

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

    // D3/D4：overlay 与透明对象在天空之后绘制（getSortedDrawList 已排序，
    // 此处取 overlay + 透明子集；不透明对象由 CNormalPass 负责）
    const auto camPos = pScene->GetActiveCamera()->getPossition();
    auto mpMatrix = pScene->GetActiveCamera()->GetVPMatrix();
    for (auto& obj : pScene->getSortedDrawList(camPos)) {
        if (!Scene::isTransparentDraw(obj) && !obj->isOverlayDraw()) continue;
        obj->draw(mpMatrix);
    }
}
