#include "RenderSystem.h"
#include "render/scene.h"
#include "pass/SkyPass.h"
#include "pass/ShadowPass.h"
#include "pass/NormalPass.h"
#include "pass/PickPass.h"
#include "image.h"
#include "camera/camera_old.h"
#include "camera/CameraController.h"

RenderSystem& RenderSystem::getInstance() {
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem() {
    mCurScene = std::make_shared<Scene>();
    mRenderContent = std::make_shared<RenderContext>();
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
    cameraController->onMouseMiddleScroll(x, y);
}

void RenderSystem::onMouseLeftDown(double x, double y) {
    auto cameraController = mCurScene->GetActiveCameraController();
    cameraController->onMouseLeftDown(x, y);
}

void RenderSystem::onMouseLeftUp(double x, double y) {
    auto cameraController = mCurScene->GetActiveCameraController();
    cameraController->onMouseLeftUp(x, y);
}

void RenderSystem::onMouseRightDown(double x, double y) {
    auto cameraController = mCurScene->GetActiveCameraController();
    cameraController->onMouseRightDown(x, y);
}

void RenderSystem::onMouseRightUp(double x, double y) {
    auto cameraController = mCurScene->GetActiveCameraController();
    cameraController->onMouseRightUp(x, y);
}

void RenderSystem::onMouseMiddleDown(double x, double y) {
    auto cameraController = mCurScene->GetActiveCameraController();
    cameraController->onMouseMiddleDown(x, y);
}

void RenderSystem::onMouseMove(double x, double y) {
    auto cameraController = mCurScene->GetActiveCameraController();
    cameraController->onMouseMove(x, y);
}

void RenderSystem::onWindowSizeChanged(int x, int y) {
    mRenderContent->screenWidth = x;
    mRenderContent->screenHeight = y;
    
    auto camera = mCurScene->GetActiveCamera();
    camera->setViewport(x, y);
}
