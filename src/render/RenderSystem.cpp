#include "RenderSystem.h"
#include "render/scene.h"
#include "image.h"

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
    std::filesystem::path pathTex(rootPath + "res");
    loadTexture(pathTex);

    // ������ɫ��
    ShaderCache::GetInstance().init(rootPath + "/res/shaders/");

	// ��ʼ������
	auto a = Scene::getScene();
}

void RenderSystem::update() {

}

void RenderSystem::draw() {
	Scene::getScene().update();
	Scene::getScene().drawShadow();
	Scene::getScene().draw();
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