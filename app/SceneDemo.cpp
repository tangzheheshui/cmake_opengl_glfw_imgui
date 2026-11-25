//
//  SceneDemo.cpp
//
#include "SceneDemo.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <vector>

#include "RenderSystem.h"
#include "scene.h"
#include "object/Line.h"
#include "object/Sky.h"
#include "object/Text.h"
#include "object/ImageRectangle.h"
#include "model/model.h"
#include "Light.h"
#include "taskQueue.h"
#include "camera/camera.h"
#include "fs/FilePathUtil.h"

namespace SceneDemo {

void buildScene(const std::string& rootPath, bool useVulkan) {
    auto pScene = RenderSystem::getInstance().GetCurScene();
    if (!pScene) return;

    // E5 流式加载：后台预解析模型文件（必须先于首个 LoadFile——
    // 解析与主循环组装并行，LoadFile 汇合等待；VK 后端）
    if (std::getenv("RHI_ASYNC_DEMO")) {
        Model::prefetchAsync(engine::fs::utf8FromPath(
            engine::fs::pathFromUtf8(rootPath) / "res" / "model" / "duck.dae"));
    }

    // 坐标轴
    std::vector<unsigned int> indices = {0, 1};
    const float len = 15;
    const glm::vec3 pZero = {0, 0, 0};
    const glm::vec3 pX = {len, 0, 0};
    const glm::vec3 pY = {0, len, 0};
    const glm::vec3 pZ = {0, 0, len};
    auto line_x = std::make_shared<Line>();
    auto line_y = std::make_shared<Line>();
    auto line_z = std::make_shared<Line>();
    line_x->setData({pZero, pX}, indices);
    line_y->setData({pZero, pY}, indices);
    line_z->setData({pZero, pZ}, indices);
    line_x->setColor({1, 0, 0});
    line_y->setColor({0, 1, 0});
    line_z->setColor({0, 0, 1});
    // D1 收尾验证开关：RHI_LINE_WIDTH=<px> 加宽坐标轴线（观察 AA 边缘与圆头）
    if (const char* w = std::getenv("RHI_LINE_WIDTH")) {
        const float lw = static_cast<float>(std::atof(w));
        line_x->setWidth(lw);
        line_y->setWidth(lw);
        line_z->setWidth(lw);
    }

    const auto start = std::chrono::high_resolution_clock::now();
    // 地面（GL 专属：calculate 混合 CPU/GPU 操作，VK 模式跳过）
    std::shared_ptr<ImageRectangle> objGround = nullptr;
    if (!useVulkan) {
        objGround = std::make_shared<ImageRectangle>();
    }
    const float ground_width = 10;
    const auto texPath = engine::fs::pathFromUtf8(rootPath) / "res" / "textures";
    if (objGround) {
        objGround->setImagePath(engine::fs::utf8FromPath(texPath / "bricks2.jpg"),
            engine::fs::utf8FromPath(texPath / "bricks2_normal.jpg"),
            engine::fs::utf8FromPath(texPath / "bricks2_disp.jpg"));
        objGround->setSetp(5, 5);
        const glm::vec3 p1(-ground_width, 0,  ground_width);
        const glm::vec3 p2( ground_width, 0,  ground_width);
        const glm::vec3 p3( ground_width, 0, -ground_width);
        const glm::vec3 p4(-ground_width, 0, -ground_width);
        objGround->setPoints(p1, p2, p3, p4);
        objGround->calculate();
    }

    // 鸭子群
    auto objDuck = std::make_shared<Model>();
    objDuck->LoadFile(engine::fs::utf8FromPath(engine::fs::pathFromUtf8(rootPath) / "res" / "model" / "duck.dae"));
    TaskQueue::instance().pushTask([start](){});
    objDuck->setCount(4);
    objDuck->setPosition(0, {0, 0, 2});
    objDuck->setScale(0, 0.01);
    objDuck->setPosition(1, {-2, 0, 2});
    objDuck->setScale(1, 0.01);
    objDuck->setPosition(2, {2, 0, 2});
    objDuck->setScale(2, 0.01);
    objDuck->setPosition(3, {0, 0, -2});
    objDuck->setScale(3, 0.01);
    objDuck->setRotateY(3, 90);

    // 光源模型
    auto objLight = std::make_shared<Model>();
    objLight->LoadFile(engine::fs::utf8FromPath(engine::fs::pathFromUtf8(rootPath) / "res" / "model" / "duck.dae"));
    objLight->setCount(1);
    objLight->setLightOpen(false);
    const auto lightPos = Light::GlobalLight().position;
    objLight->setPosition(0, {lightPos.x, lightPos.y, lightPos.z});
    objLight->setScale(0, 0.5);

    // 天空盒
    const auto skyboxPath = texPath / "skybox";
    auto objSky = std::make_shared<Sky>();
    objSky->setCubeImage({
        engine::fs::utf8FromPath(skyboxPath / "right.jpg"),
        engine::fs::utf8FromPath(skyboxPath / "left.jpg"),
        engine::fs::utf8FromPath(skyboxPath / "top.jpg"),
        engine::fs::utf8FromPath(skyboxPath / "bottom.jpg"),
        engine::fs::utf8FromPath(skyboxPath / "front.jpg"),
        engine::fs::utf8FromPath(skyboxPath / "back.jpg")
    });

    // V3-20 阴影演示（RHI_SHADOW_DEMO=1）：在光源下方放一个投影物，
    // 让它的影子落在 VK 模式的地面上。默认场景不变。
    std::shared_ptr<Model> objShadowCaster;
    if (std::getenv("RHI_SHADOW_DEMO")) {
        objShadowCaster = std::make_shared<Model>();
        objShadowCaster->LoadFile(engine::fs::utf8FromPath(
            engine::fs::pathFromUtf8(rootPath) / "res" / "model" / "duck.dae"));
        objShadowCaster->setCount(1);
        objShadowCaster->setPosition(0, {0, 5, 0});
        objShadowCaster->setScale(0, 0.3f);
        pScene->AddObj(objShadowCaster);
    }

    pScene->AddObj(objDuck);
    pScene->AddObj(objLight);
    pScene->AddObj(line_x);
    pScene->AddObj(line_y);
    pScene->AddObj(line_z);
    pScene->SetSkyBox(objSky);

    // C2 收尾验证开关：RHI_VIEWPORT_CAMS=1 加三个辅助相机（配 RHI_VIEWPORTS=4
    // 时 tile0=主相机 / tile1=右视 / tile2=顶视 / tile3=后视）
    if (std::getenv("RHI_VIEWPORT_CAMS")) {
        auto right = std::make_shared<Camera>();
        right->setPosition({40, 0, 0});
        auto top = std::make_shared<Camera>();
        top->setPosition({0, 40, 0});
        auto back = std::make_shared<Camera>();
        back->setPosition({-40, 0, 0});
        pScene->addViewportCamera(right);
        pScene->addViewportCamera(top);
        pScene->addViewportCamera(back);
    }

    // D3 验证开关：RHI_ALPHA_DEMO=1 加一只透明鸭子（默认关，不影响像素门禁；
    // RHI_ALPHA=<0..1> 覆盖透明度，1.0 用于隔离几何/混合问题）
    if (std::getenv("RHI_ALPHA_DEMO")) {
        auto objAlpha = std::make_shared<Model>();
        objAlpha->LoadFile(engine::fs::utf8FromPath(
            engine::fs::pathFromUtf8(rootPath) / "res" / "model" / "duck.dae"));
        objAlpha->setCount(1);
        objAlpha->setPosition(0, {-4, 0, 2});
        objAlpha->setScale(0, 0.01);
        const float a = std::getenv("RHI_ALPHA") ? static_cast<float>(std::atof(std::getenv("RHI_ALPHA")))
                                                 : 0.45f;
        objAlpha->setAlpha(a);
        pScene->AddObj(objAlpha);
    }

    // 文字（D4：两端走 SDF RHI 图集路径；GL 可经 textUseRHI 切回 legacy 逐字形）
    {
        if (std::getenv("RHI_TEXT_RHI")) {   // GL 调试开关：强制文字走 RHI 路径
            RenderSystem::getInstance().settings().textUseRHI = true;
        }
        if (const char* m = std::getenv("RHI_DISPLAY_MODE")) {   // D5 验证开关：0/1/2
            RenderSystem::getInstance().settings().displayMode = std::atoi(m);
        }
        if (std::getenv("RHI_XRAY")) {   // X-ray 验证开关
            auto& st = RenderSystem::getInstance().settings();
            st.xrayEnabled = true;
            if (const char* o = std::getenv("RHI_XRAY_OPACITY")) {
                st.xrayOpacity = static_cast<float>(std::atof(o));
            }
        }
        if (std::getenv("RHI_HOVER_OFF")) {   // 悬停拾取关闭（压测/对比用）
            RenderSystem::getInstance().settings().hoverEnabled = false;
        }
        if (std::getenv("RHI_CULL_OFF")) {    // E3：关闭视锥剔除（性能对比）
            RenderSystem::getInstance().settings().frustumCull = false;
        }
        if (std::getenv("RHI_CULL_DEMO") && !std::getenv("RHI_CULL_OFF")) {
            // off 基线不加载幕后鸭（几何差异二分：剔除空对象 vs 不加载）
            auto objBehind = std::make_shared<Model>();
            objBehind->LoadFile(engine::fs::utf8FromPath(
                engine::fs::pathFromUtf8(rootPath) / "res" / "model" / "duck.dae"));
            objBehind->setCount(1);
            objBehind->setPosition(0, {0, 0, 50});   // 相机 (0,0,40) 看 -z，z=50 在背后
            objBehind->setScale(0, 0.01);
            pScene->AddObj(objBehind);
        }
        auto fontMng = RenderSystem::getInstance().GetFontMng();
        auto objText = std::make_shared<CText>(std::weak_ptr<FontManager>(fontMng));
        objText->SetText("test");
        pScene->AddObj(objText);
    }
}

} // namespace SceneDemo
