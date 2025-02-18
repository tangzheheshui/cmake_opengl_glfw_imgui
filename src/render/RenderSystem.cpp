#include "RenderSystem.h"
#include "render/scene.h"
#include "render/pass/SkyPass.h"
#include "render/pass/ShadowPass.h"
#include "render/pass/NormalPass.h"
#include "image.h"

const unsigned int SCR_WIDTH = 800;
const unsigned int SCR_HEIGHT = 600;

RenderSystem& RenderSystem::getInstance() {
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem() {

}

RenderSystem::~RenderSystem() {

}

void RenderSystem::init(const std::string& rootPath) {
    // ��������
    TextureMng::getInstance().setRootPath(rootPath + "/res/textures");
    std::filesystem::path pathTex(rootPath + "/res");
    loadTexture(pathTex);

    // ������ɫ��
    ShaderCache::GetInstance().init(rootPath + "/res/shaders/");

	// ��ʼ������
    Scene::getScene().setRootPath(rootPath);
    Scene::getScene().init();
    
    //mCurScene = Scene::getScene();
    // 创建pass
    auto skyPass = std::make_shared<CSkyPass>();
    m_vec_renderpass.push_back(skyPass);

    auto shadowPass = std::make_shared<CShadowPass>();
    m_vec_renderpass.push_back(shadowPass);

    auto normalPass = std::make_shared<CNormalPass>();
    m_vec_renderpass.push_back(normalPass);
}

void RenderSystem::update() {

}

void RenderSystem::BeginDraw() {

}

void RenderSystem::draw() {
	//Scene::getScene().update();
	//Scene::getScene().drawShadow();
	//Scene::getScene().draw();

    // 新架构的代码
    for (auto pass : m_vec_renderpass)
    {
        pass->draw(&Scene::getScene());
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
