// RenderSYstem

#pragma once 
#include <memory>
#include <string>
#include <vector>
#include <filesystem>

class TextureMng;
class ShaderMng;
class Scene;
class RenderPass;
class IRenderPass;

class RenderSystem {
public:
	static RenderSystem& getInstance();
	RenderSystem(const RenderSystem&) = delete;
	RenderSystem& operator=(const RenderSystem&) = delete;

	void init(const std::string &rootPath);
	void update();
    void BeginDraw();
	void draw();
private:
	RenderSystem();
	~RenderSystem();
	void loadTexture(const std::filesystem::path& dirPath);
private:
	std::shared_ptr<TextureMng> m_texMng;
	std::shared_ptr<ShaderMng> m_shaderMng;
	std::shared_ptr<Scene> mCurScene;
	std::vector<std::shared_ptr<IRenderPass>> m_vec_renderpass;
}; 
