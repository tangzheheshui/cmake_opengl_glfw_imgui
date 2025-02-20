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
class RenderContext;

struct Point2D {
    double x{};
    double y{};
};

struct MouseState {
    Point2D pressPos;
    Point2D releasePos;
    Point2D lastPos;
    std::chrono::high_resolution_clock::time_point pressTime;
    bool isDragging = false;
    bool isMouseLeftPressed = false;
};

class RenderSystem {
public:
	static RenderSystem& getInstance();
	RenderSystem(const RenderSystem&) = delete;
	RenderSystem& operator=(const RenderSystem&) = delete;

	void init(const std::string &rootPath);
	void update();
    void BeginDraw();
	void draw();
#pragma region mouse_event
    void onMouseMiddleScroll(double x, double y); // 滚轮滚动
    void onMouseLeftDown(double x, double y);    // 左键down
    void onMouseLeftUp(double x, double y);      // 左键up
    void onMouseRightDown(double x, double y);   // 右键down
    void onMouseRightUp(double x, double y);     // 右键up
    void onMouseMiddleDown(double x, double y);  // 滚轮点击
    void onMouseMove(double x, double y);       // 鼠标移动
#pragma endregion
    void onWindowSizeChanged(int x, int y);
private:
	RenderSystem();
	~RenderSystem();
	void loadTexture(const std::filesystem::path& dirPath);
private:
	std::shared_ptr<TextureMng> m_texMng;
	std::shared_ptr<ShaderMng> m_shaderMng;
	std::shared_ptr<Scene> mCurScene;
	std::vector<std::shared_ptr<IRenderPass>> m_vec_renderpass;
    std::shared_ptr<RenderContext> mRenderContent;
    MouseState mMouseState;
}; 
