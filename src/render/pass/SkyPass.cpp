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

    // D3：透明对象在天空之后按视深远 → 近绘制（getSortedDrawList 已排序，
    // 此处只取透明子集；不透明对象由 CNormalPass 负责）
    const auto camPos = pScene->GetActiveCamera()->getPossition();
    auto mpMatrix = pScene->GetActiveCamera()->GetVPMatrix();
    for (auto& obj : pScene->getSortedDrawList(camPos)) {
        if (obj->getAlpha() >= 0.999f) continue;
        obj->draw(mpMatrix);
    }
}
