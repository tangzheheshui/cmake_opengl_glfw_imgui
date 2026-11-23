#include "NormalPass.h"
#include "color/ColorSpace.h"
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
    // B3c：清屏色是作者指定的 sRGB 值，sRGB 帧缓冲会把它当线性值编码，
    // 故先转线性再清除（顺带修正原来的 glClear 早于 glClearColor 的顺序）。
    glClearColor(color::srgbToLinear(0.2f), color::srgbToLinear(0.3f),
                 color::srgbToLinear(0.3f), 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // 矩阵
    auto mpMatrix = pScene->GetActiveCamera()->GetVPMatrix();
    
    for (auto obj : pScene->GetObjs()) {
        obj->draw(mpMatrix);
    }
}
