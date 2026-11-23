// RenderSYstem

#pragma once
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include "RenderSettings.h"
#include "rhi/RhiTypes.h"

class TextureMng;
class Scene;
class MeshPass;
class IRenderPass;
class RenderContext;
class FontManager;
struct GLFWwindow;
namespace rhi { class IRHIDevice; class IRHICommandList; }

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
	// 必须在 glfwTerminate() 之前显式调用：RHI 设备/GL 资源的析构依赖窗口与
	// 图形上下文存活（单例静态析构发生在 main 返回、窗口销毁之后）。
	void shutdown();
	void update();
    void BeginDraw();
	void draw();
public:
    // ---- RHI（引擎层，与 UI 解耦）----
    // 必须在 init() 之前调用（GL 后端复用主窗口上下文）
    void setWindow(GLFWwindow* window) { _window = window; }
    RenderSettings& settings() { return m_settings; }            // 渲染设置（UI 层可读写）
    rhi::IRHIDevice* rhiDevice() { return m_rhiDevice.get(); }   // RHI 设备（可能为空，UI/业务层只读使用）
    // 资源预载（app 层显式调用，引擎不假设项目资源布局）
    void preloadTextures(const std::string& dirPath);
    // 叠加渲染回调：在 RHI render pass 内、场景之后调用（ImGui Vulkan 等 UI 层用）
    void setOverlayRenderer(std::function<void(rhi::IRHICommandList*)> fn) { m_overlayFn = std::move(fn); }
public:
    std::shared_ptr<Scene> GetCurScene() { return mCurScene; }
    std::shared_ptr<FontManager> GetFontMng() { return m_fontMng; }
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
private:
	void createSmokeTriangle();     // RHI 冒烟资源初始化
	void drawSmokeTriangle();       // RHI 冒烟绘制（GL 叠加路径：自建 frame/pass）
	void createTexturedQuad();      // M4 贴图四边形资源
	void drawTexturedQuad();        // M4 贴图四边形绘制（GL 叠加路径：自建 frame/pass）
	// 在当前已开启的 render pass 内录制（VK 场景路径复用，不自行 beginFrame）
	void recordSmokeTriangle(rhi::IRHICommandList* cmd);
	void recordTexturedQuad(rhi::IRHICommandList* cmd);
	void renderScenePasses();       // 帧级 Pass 循环
	void drawSceneOverlays();       // RHI 叠加内容（冒烟三角形/贴图四边形）
	void drawRhiScene();            // Vulkan 后端：RHI 场景绘制
	std::function<void(rhi::IRHICommandList*)> m_overlayFn;
	void selfTestAB();              // M4.2 验证设施：同帧 A/B 逐像素对比
	// B4 验证设施（RHI_RT_TEST=1）：离屏 RT 渲染冒烟三角形 → 回读中心/角落像素。
	// 两阶段状态机：第 N 帧渲染、第 N+1 帧回读（VK 上一帧已提交完成）
	void runRenderTargetReadbackTest();
	rhi::RenderTargetHandle m_rtTestTarget;
	void loadTexture(const std::filesystem::path& dirPath);
    std::shared_ptr<TextureMng> m_texMng;
    std::shared_ptr<FontManager> m_fontMng;
	std::shared_ptr<Scene> mCurScene;
	std::vector<std::shared_ptr<IRenderPass>> m_vec_renderpass;
    std::shared_ptr<RenderContext> mRenderContent;
    MouseState mMouseState;

    // ---- RHI（新增，纯增量）----
    GLFWwindow* _window = nullptr;
    rhi::Backend _backend = rhi::Backend::OpenGL;   // 启动时由 RHI_BACKEND 环境变量决定
    RenderSettings m_settings;
    std::unique_ptr<rhi::IRHIDevice> m_rhiDevice;
    // 冒烟三角形资源（验证 RHI 链路，业务无关）
    rhi::BufferHandle m_smokeVB;
    rhi::BufferHandle m_smokeIB;
    rhi::PipelineHandle m_smokePipeline;
    // 贴图四边形（M4 纹理链路验证）
    rhi::BufferHandle m_quadVB;
    rhi::BufferHandle m_quadIB;
    rhi::TextureHandle m_quadTex;
    rhi::PipelineHandle m_quadPipeline;
    // 四边形专属 uniform 块：避免 bindTexture 污染上一个模型对象的描述符集
    rhi::UniformBlockHandle m_quadUB;
}; 
