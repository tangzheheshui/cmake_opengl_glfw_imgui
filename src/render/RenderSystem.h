// RenderSYstem

#pragma once 
#include <memory>
#include <string>
#include <filesystem>

class TextureMng;
class ShaderMng;
class Scene;
class RenderPass;

class RenderSystem {
public:
	static RenderSystem& getInstance();
	RenderSystem(const RenderSystem&) = delete;
	RenderSystem& operator=(const RenderSystem&) = delete;

	void init(const std::string &rootPath);

	void update();
	void draw();
private:
	RenderSystem();
	~RenderSystem();
	void loadTexture(const std::filesystem::path& dirPath);
private:
	std::shared_ptr<TextureMng> m_texMng;
	std::shared_ptr<ShaderMng> m_shaderMng;
	std::shared_ptr<Scene> m_scene;
	std::shared_ptr<RenderPass> m_vec_renderpass;
}; 