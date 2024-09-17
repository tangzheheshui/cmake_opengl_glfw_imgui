// RenderSYstem

#pragma once 
#include <memory>

class TextureMng;
class ShaderMng;
class Scene;
class RenderPass;

class RenderSystem {
public:
	static RenderSystem& getInstance();
	RenderSystem(const RenderSystem&) = delete;
	RenderSystem& operator=(const RenderSystem&) = delete;

	void init();

	void update();
	void draw();
private:
	RenderSystem();
	~RenderSystem();
private:
	std::shared_ptr<TextureMng> m_texMng;
	std::shared_ptr<ShaderMng> m_shaderMng;
	std::shared_ptr<Scene> m_scene;
	std::shared_ptr<RenderPass> m_vec_renderpass;
}; 