#include "RenderSystem.h"
#include "render/scene.h"
#include "pass/SkyPass.h"
#include "pass/ShadowPass.h"
#include "pass/NormalPass.h"
#include "pass/PickPass.h"
#include "image.h"
#include "camera/camera_old.h"
#include "camera/CameraController.h"
#include "FontManager.h"

RenderSystem& RenderSystem::getInstance() {
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem() {
    mCurScene = std::make_shared<Scene>();
    mRenderContent = std::make_shared<RenderContext>();
    m_fontMng = std::make_shared<FontManager>();
}

RenderSystem::~RenderSystem() {

}

void RenderSystem::init(const std::string& rootPath) {
    // 加载纹理
    auto pathTex = std::filesystem::path(rootPath) / "res";
    loadTexture(pathTex);

    // ������ɫ��
    auto shaderPath = std::filesystem::path(rootPath) / "res" / "shaders";
    ShaderCache::GetInstance().init(shaderPath.string() + std::string(1, std::filesystem::path::preferred_separator));

	// ��ʼ������
    mCurScene->setRootPath(rootPath);
    mCurScene->init();
    
    // 创建pass
    auto shadowPass = std::make_shared<CShadowPass>(mRenderContent);
    m_vec_renderpass.push_back(shadowPass);

    auto normalPass = std::make_shared<CNormalPass>(mRenderContent);
    m_vec_renderpass.push_back(normalPass);
    
    auto skyPass = std::make_shared<CSkyPass>(mRenderContent);
    m_vec_renderpass.push_back(skyPass);
    
    auto pickPass = std::make_shared<CPickPass>(mRenderContent);
    m_vec_renderpass.push_back(pickPass);

    // 加载字体
    auto fontPath = std::filesystem::path(rootPath) / "res" / "font" / "arial.ttf";
    m_fontMng->LoadFont(fontPath.string(), 48);
}

void RenderSystem::update() {

}

void RenderSystem::BeginDraw() {

}

void RenderSystem::draw() {
    // 新架构的代码
    for (auto pass : m_vec_renderpass)
    {
        pass->draw(mCurScene.get());
    }
}

void RenderSystem::loadTexture(const std::filesystem::path& dirPath) {
    for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
        if (entry.is_directory()) {
            loadTexture(entry.path());
        }
        else if (entry.is_regular_file()) {
            auto extension = entry.path().extension().string();
            if (extension == ".png" || extension == ".jpg" || extension == ".tga") {
                auto filename = entry.path().string();
                TextureMng::getInstance().loadTexture(filename);
            }
        }
    }
}

void RenderSystem::onMouseMiddleScroll(double x, double y) {
    auto cameraController = mCurScene->GetActiveCameraController();
    cameraController->onMouseScale(y);
}

void RenderSystem::onMouseLeftDown(double x, double y) {
    mMouseState.isMouseLeftPressed = true;
    mMouseState.pressPos =  mMouseState.lastPos = {x, y};
    mMouseState.pressTime = std::chrono::high_resolution_clock::now();
}

void RenderSystem::onMouseLeftUp(double x, double y) {
    mMouseState.isMouseLeftPressed = false;
    
    // 点击的时间间隔
    auto releaseTime = std::chrono::high_resolution_clock::now();
    double duration = std::chrono::duration<double, std::milli>(releaseTime - mMouseState.pressTime).count();

    // 计算移动距离
    double dx = x - mMouseState.pressPos.x;
    double dy = y - mMouseState.pressPos.y;
    double distance = std::sqrt(dx * dx + dy * dy);

    // 判断是否为 Click（时间短 & 移动距离小）
    if (duration < 200 && distance < 5) {  // 200ms内 & 移动距离 < 5 像素
        printf("Click detected at (%.1f, %.1f)\n", x, y);
    } 
}

void RenderSystem::onMouseRightDown(double x, double y) {
}

void RenderSystem::onMouseRightUp(double x, double y) {
}

void RenderSystem::onMouseMiddleDown(double x, double y) {
}

void RenderSystem::onMouseMove(double x, double y) {
    if (!mMouseState.isMouseLeftPressed) {
        return;
    }

    float xoffset = x - mMouseState.lastPos.x;
    float yoffset = y - mMouseState.lastPos.y;
    mMouseState.lastPos = {x, y};

    float sensitivity = 0.1f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;
    
    auto cameraController = mCurScene->GetActiveCameraController();
    cameraController->onMouseDrag(xoffset, yoffset);
}

void RenderSystem::onWindowSizeChanged(int x, int y) {
    mRenderContent->screenWidth = x;
    mRenderContent->screenHeight = y;
    
    auto camera = mCurScene->GetActiveCamera();
    camera->setViewport(x, y);
}
