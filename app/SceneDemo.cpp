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

    pScene->AddObj(objDuck);
    pScene->AddObj(objLight);
    pScene->AddObj(line_x);
    pScene->AddObj(line_y);
    pScene->AddObj(line_z);
    pScene->SetSkyBox(objSky);

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

    // 文字（GL 专属：字形纹理为 GL 实现，VK 文字渲染待迁移）
    if (!useVulkan) {
        auto fontMng = RenderSystem::getInstance().GetFontMng();
        auto objText = std::make_shared<CText>(std::weak_ptr<FontManager>(fontMng));
        objText->SetText("test");
        pScene->AddObj(objText);
    }
}

} // namespace SceneDemo
