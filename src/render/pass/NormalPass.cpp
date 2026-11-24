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

    // D3：本 Pass 只画不透明对象（透明在 CSkyPass 的天空之后画——天空盒
    // 深度测试会覆盖不写深度的透明像素，顺序契约 = 不透明 → 天空 → 透明）
    const auto camPos = pScene->GetActiveCamera()->getPossition();
    for (auto& obj : pScene->getSortedDrawList(camPos)) {
        if (obj->getAlpha() < 0.999f) continue;
        obj->draw(mpMatrix);
    }
}
