// RenderSYstem

#pragma once
#include <memory>
#include <string>
#include <vector>
#include <filesystem>
#include <functional>
#include "core/threadPool.h"
#include "RenderSettings.h"
#include "rhi/RhiTypes.h"

class TextureMng;
class Scene;
class MeshPass;
class FontManager;
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

#include <glm/glm.hpp>
class Matrix;

class RenderSystem {
public:
	static RenderSystem& getInstance();
	RenderSystem(const RenderSystem&) = delete;
	RenderSystem& operator=(const RenderSystem&) = delete;

	// ---- C1b：门面初始化（宿主拥有设备与窗口，引擎只借用）----
	struct InitDesc {
		// 两种所有权模式（二选一）：
		//  1) device != nullptr：宿主已建好设备并持有，引擎只借用、不销毁；
		//  2) device == nullptr：引擎用下面的 deviceDesc 自建设备并负责销毁。
		// 宿主通常用 2，app 层不再需要认识 RHIFactory / IRHIDevice 生命周期。
		rhi::IRHIDevice* device = nullptr;
		rhi::DeviceDesc  deviceDesc;         // 模式 2 使用（窗口/后端/着色器路径）
		std::string assetRoot;               // 含 res/ 的根目录（UTF-8）
		uint32_t frameWidth  = 0;            // 帧缓冲像素尺寸
		uint32_t frameHeight = 0;
		uint32_t windowWidth  = 0;           // 窗口逻辑尺寸（拾取坐标换算用；
		uint32_t windowHeight = 0;           //   0 = 等于帧缓冲，scale=1）
	};
	// 生命周期约定（宿主必须遵守）：
	//   窗口创建 → GL 上下文 current（GL）/ 设备创建 → renderer.init
	//     → 每帧 renderer.renderFrame()
	//     → renderer.shutdown() → 宿主销毁 device → 窗口销毁
	bool init(const InitDesc& desc, std::string* outError = nullptr);
	// 只销毁引擎自己的资源，不销毁 device（宿主在窗口销毁前自行销毁设备；
	// 单例静态析构发生在 main 返回之后，此时销毁依赖图形上下文的资源会崩溃）
	void shutdown();
	void update();
    void BeginDraw();
	// 完整一帧：beginFrame → passes → overlay → endFrame → present。
	// GL/VK 主循环零后端分支（ImGui backend 初始化除外）
	void renderFrame();
	void draw();   // 过渡别名：内部转调 renderFrame()（保留一个迭代）
public:
    // ---- RHI（引擎层，与 UI 解耦）----
    RenderSettings& settings() { return m_settings; }            // 渲染设置（UI 层可读写）
    rhi::IRHIDevice* rhiDevice() { return m_rhiDevice; }         // 只读借用（可能为空）
    // V3-19：阴影贴图 + 光空间矩阵（MeshPass 主场景着色器采样用）
    rhi::TextureHandle shadowTexture() const { return m_shadowTex; }
    // E4：LOD 距离判定用——当前场景相机位置（无场景时退回全局相机）
    glm::vec3 activeCameraPos() const;
    Matrix             shadowLightVP() const;
    // 资源预载（app 层显式调用，引擎不假设项目资源布局）
    void preloadTextures(const std::string& dirPath);
    // 叠加渲染回调（UI 合成点）：VK 在 render pass 内、场景之后调用；
    // GL 在默认帧缓冲、GL_FRAMEBUFFER_SRGB 关闭后调用（ImGui GL 等）
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
    void onWindowSizeChanged(int x, int y);   // 窗口尺寸变化（宿主回调转发）
private:
	RenderSystem();
	~RenderSystem();
private:
	void createSmokeTriangle();     // RHI 冒烟资源初始化
	void createTexturedQuad();      // M4 贴图四边形资源
	// 在当前已开启的 render pass 内录制（场景路径复用，不自行 beginFrame）
	void recordSmokeTriangle(rhi::IRHICommandList* cmd);
	void recordTexturedQuad(rhi::IRHICommandList* cmd);
	void drawRhiSceneContents();    // 场景内容（GL/VK 同一套 RHI 序列）
	void ensureSecondaryLists(int count);   // 惰性创建 secondary 列表（多线程录制）
	std::function<void(rhi::IRHICommandList*)> m_overlayFn;
	std::vector<rhi::IRHICommandList*> m_secondaryLists;   // 多线程录制 secondary 列表（惰性创建）
	std::unique_ptr<ThreadPool> m_threadPool;              // 持久录制线程池（惰性创建）
	// B4 验证设施（RHI_RT_TEST=1）：离屏 RT 渲染冒烟三角形 → 回读中心/角落像素。
	// 两阶段状态机：第 N 帧渲染、第 N+1 帧回读（VK 上一帧已提交完成）
	void runRenderTargetReadbackTest();
	rhi::RenderTargetHandle m_rtTestTarget;

	// D2: RHI 拾取（ID 渲染 + 回读）。requestPick 记录点击点，第 N 帧渲染 ID 图、
	// 第 N+1 帧回读 1px 解码对象 ID。RHI_PICK_TEST=1 启动后自动在屏幕中心拾取一次。
	void ensurePickResources();
	void requestPick(double x, double y, bool isHover = false);
	void runPickPass();
	rhi::PipelineHandle m_pickPipeline;
	rhi::RenderTargetHandle m_pickRT;

	// V3-18：阴影 Pass——纯深度目标（1024²）+ 深度专用管线
	rhi::PipelineHandle     m_shadowPipeline;
	// MSAA RT 验证设施（RHI_RT_MSAA=<n>）：同款冒烟管线，采样数为 n
	rhi::PipelineHandle     m_smokePipelineMsaa;
	uint32_t                m_rtMsaaSamples = 1;
	rhi::RenderTargetHandle m_shadowRT;
	bool                    m_shadowInitTried = false;
	void ensureShadowResources();
	void recordSmokeTriangleWith(rhi::IRHICommandList* cmd, rhi::PipelineHandle pipeline);
	void recordShadowPass(rhi::IRHICommandList* cmd);
	rhi::TextureHandle m_shadowTex;
	// V3-20：阴影接收面（两端统一走 RHI，GL 不再用遗留 ImageRectangle）
	rhi::BufferHandle   m_groundVB;
	rhi::PipelineHandle m_groundPipeline;
	void recordGroundRHI(rhi::IRHICommandList* cmd, const Matrix& viewProj);
	struct PickRequest {
		bool  pending = false;   // 已请求、待渲染
		bool  rendered = false;  // 渲染完成、待回读
		bool  isHover = false;   // 悬停拾取（命中写 hoverId 而非 selectedId）
		uint32_t x = 0, y = 0;   // 回读点（RT 像素坐标）
	};
	PickRequest m_pickReq;
	// 悬停拾取节流（D8 扩展）：光标移动 ≥ 阈值且间隔 ≥ 帧数才请求，
	// 避免每次 mousemove 都渲染整幅 ID 图
	double   m_hoverLastX = -1e9, m_hoverLastY = -1e9;
	int      m_hoverCooldown = 0;
	void loadTexture(const std::filesystem::path& dirPath);
	// V3-0 验证设施（RHI_CHURN=<N>）：N 轮创建/销毁纹理，压测延迟销毁队列
	// 与内存分配器账目（VK 配合验证层观察 0 error / 退出 live=0）
	void runChurnTest();
    std::shared_ptr<TextureMng> m_texMng;
    std::shared_ptr<FontManager> m_fontMng;
	std::shared_ptr<Scene> mCurScene;
    MouseState mMouseState;

    // ---- RHI（C1b 门面）----
    rhi::Backend _backend = rhi::Backend::OpenGL;   // 由 device->backend() 得到
    RenderSettings m_settings;
    rhi::IRHIDevice* m_rhiDevice = nullptr;   // 借用或自持（见 InitDesc 两种模式）
    std::unique_ptr<rhi::IRHIDevice> m_ownedDevice;   // 模式 2：引擎自建并销毁
    // 窗口→帧缓冲坐标换算（拾取；InitDesc 提供逻辑尺寸）
    uint32_t m_frameW = 0, m_frameH = 0;
    uint32_t m_windowW = 0, m_windowH = 0;
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
