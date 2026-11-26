#include "log/LogMacros.h"
#include "RenderSystem.h"
#include "rhi/RHIFactory.h"
#include "render/scene.h"
#include "render/Light.h"
#include <glm/gtc/type_ptr.hpp>
#include "object/Sky.h"
#include "model/model.h"
#include "image.h"
#include "camera/camera.h"
#include "camera/CameraController.h"
#include "FontManager.h"
#include "rhi/RHIFactory.h"
#include "rhi/IRHIDevice.h"
#include "rhi/IRHICommandList.h"
#include "color/ColorSpace.h"
#include "fs/FilePathUtil.h"
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <chrono>
#include <thread>

// ---- RHI 冒烟三角形几何（NDC 位置 + 颜色，交错存放）----
namespace {
struct SmokeVertex {
    float pos[3];
    float col[3];
};
const SmokeVertex kSmokeVertices[3] = {
    {{-0.6f, -0.6f, 0.0f}, {1.0f, 0.2f, 0.2f}},   // 红
    {{ 0.6f, -0.6f, 0.0f}, {0.2f, 1.0f, 0.2f}},   // 绿
    {{ 0.0f,  0.6f, 0.0f}, {0.2f, 0.4f, 1.0f}},   // 蓝
};
const float kIdentityMat4[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};

// 帧性能统计（RHI_FRAME_STATS=1）：细分「剔除+排序」vs「录制」，定位 CPU 瓶颈。
// 累计 60 帧输出一次平均（多视口下每 tile 各累计一次，取和）。
struct FrameStats {
    bool   on = false;
    double cullMs = 0, recordMs = 0;
    size_t draws = 0;
    int    frames = 0;
    void accum(double cull, double record, size_t n) {
        if (!on) return;
        cullMs += cull; recordMs += record; draws += n;
        if (++frames % 60 == 0) {
            ENGINE_LOG_INFO("[STATS] cull+sort=%.2fms record=%.2fms draws=%zu",
                            cullMs / 60.0, recordMs / 60.0, draws / 60);
            cullMs = recordMs = 0.0; draws = 0;
        }
    }
};
FrameStats g_stats;
} // namespace

RenderSystem& RenderSystem::getInstance() {
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem() {
    mCurScene = std::make_shared<Scene>();
    m_fontMng = std::make_shared<FontManager>();
}

RenderSystem::~RenderSystem() {

}

void RenderSystem::shutdown() {
    // 只销毁引擎自己的资源（先等 GPU 收尾）；设备归宿主销毁（C1b 所有权约定）
    if (m_rhiDevice) {
        m_rhiDevice->waitIdle();
        // V3-0（来自两机合并，origin 963c854）：验证/拾取用的离屏 RT 显式释放；
        // 设备析构另有兜底回收（~VulkanDevice 的 _renderTargets 循环）
        if (m_pickRT.valid()) {
            m_rhiDevice->destroyRenderTarget(m_pickRT);
            m_pickRT = {};
        }
        if (m_rtTestTarget.valid()) {
            m_rhiDevice->destroyRenderTarget(m_rtTestTarget);
            m_rtTestTarget = {};
        }
    }
    // 依赖 GL 上下文的对象（字体纹理等）在此释放
    m_fontMng.reset();
    mCurScene.reset();
    // 模式 2：设备由引擎创建 → 在这里销毁（调用方保证仍在窗口/上下文存活期内）
    m_ownedDevice.reset();
    m_rhiDevice = nullptr;
}

bool RenderSystem::init(const InitDesc& desc, std::string* outError) {
    // 两种所有权模式：宿主给设备（借用），或给 DeviceDesc 由引擎自建
    if (desc.device) {
        m_rhiDevice = desc.device;
    } else {
        std::string devErr;
        m_ownedDevice = rhi::RHIFactory::createDevice(desc.deviceDesc, &devErr);
        if (!m_ownedDevice) {
            if (outError) *outError = devErr.empty() ? "createDevice failed" : devErr;
            return false;
        }
        m_rhiDevice = m_ownedDevice.get();
    }
    _backend    = m_rhiDevice->backend();
    m_frameW  = desc.frameWidth;
    m_frameH  = desc.frameHeight;
    m_windowW = desc.windowWidth  ? desc.windowWidth  : desc.frameWidth;
    m_windowH = desc.windowHeight ? desc.windowHeight : desc.frameHeight;
    ENGINE_LOG_INFO("[RHI] backend = %s (%s)",
                    _backend == rhi::Backend::Vulkan ? "Vulkan" : "OpenGL",
                    m_ownedDevice ? "device: engine-owned" : "device: borrowed from host");

    // E4：LOD 演示开关（默认关，不影响既有画面）
    if (const char* e = std::getenv("RHI_LOD")) {
        m_settings.lodEnabled = (std::atoi(e) != 0);
    }

    // ������ɫ��
    // A4: rootPath 约定为 UTF-8；Windows 上 path(std::string) 按 ACP 解释会错，
    // 统一走 pathFromUtf8（引擎内路径字符串 = UTF-8）
	// ��ʼ������
    mCurScene->init();

    // C1b：设备由宿主创建并传入（DeviceDesc 契约，见 app/main.cpp）；
    // 引擎不读 RHI_BACKEND、不持有窗口
    createSmokeTriangle();
    createTexturedQuad();
    runChurnTest();   // RHI_CHURN=<N> 时执行（验证设施）
    return true;
}

void RenderSystem::createSmokeTriangle() {
    if (!m_rhiDevice) return;

    rhi::GraphicsPipelineDesc pipeDesc;
    pipeDesc.shaders = {
        { rhi::ShaderStage::Vertex,   "vert_rhi_smoke" },
        { rhi::ShaderStage::Fragment, "frag_rhi_smoke" },
    };
    pipeDesc.vertexLayout.stride = sizeof(SmokeVertex);
    pipeDesc.vertexLayout.attributes = {
        { 0, rhi::VertexFormat::Float3, offsetof(SmokeVertex, pos) },
        { 1, rhi::VertexFormat::Float3, offsetof(SmokeVertex, col) },
    };
    pipeDesc.primitive   = rhi::PrimitiveType::Triangles;
    pipeDesc.cull        = rhi::CullMode::None;
    pipeDesc.depth       = { false, false };    // 测试三角形永远置顶
    pipeDesc.blend       = { false };
    m_smokePipeline = m_rhiDevice->createPipeline(pipeDesc);

    // MSAA RT 验证设施：RHI_RT_MSAA=<n> 时再建一条同款 n 采样管线
    if (const char* e = std::getenv("RHI_RT_MSAA")) {
        const uint32_t n = static_cast<uint32_t>(std::atoi(e));
        if (n > 1) {
            rhi::GraphicsPipelineDesc msaaDesc = pipeDesc;
            msaaDesc.attachment.samples = n;
            m_smokePipelineMsaa = m_rhiDevice->createPipeline(msaaDesc);
            m_rtMsaaSamples = m_smokePipelineMsaa.valid() ? n : 1;
            ENGINE_LOG_INFO("[RTTEST] MSAA pipeline samples=%u (%s)",
                            n, m_smokePipelineMsaa.valid() ? "ok" : "failed");
        }
    }

    // B3c：顶点色是作者指定的 sRGB 值；sRGB 交换链写回时编码，故先转线性
    SmokeVertex smokeVerts[3];
    for (int i = 0; i < 3; ++i) {
        smokeVerts[i] = kSmokeVertices[i];
        color::srgbToLinear(smokeVerts[i].col, 3);
    }
    rhi::BufferDesc vbDesc;
    vbDesc.size = sizeof(smokeVerts);
    vbDesc.usage = rhi::BUF_VERTEX;
    vbDesc.initialData = smokeVerts;
    m_smokeVB = m_rhiDevice->createBuffer(vbDesc);

    // 索引 0,1,2：走 drawIndexed（M3 Vulkan 验证用）
    static const unsigned int kSmokeIndices[3] = {0, 1, 2};
    rhi::BufferDesc ibDesc;
    ibDesc.size = sizeof(kSmokeIndices);
    ibDesc.usage = rhi::BUF_INDEX;
    ibDesc.initialData = kSmokeIndices;
    m_smokeIB = m_rhiDevice->createBuffer(ibDesc);
}

// ---- V3-0 验证设施：RHI_CHURN=<N> 纹理创建/销毁压测 ----
// 每轮 beginFrame → 创建 8 张 64x64 纹理 → 逐帧 destroy → endFrame；
// 结束后等 GPU 空闲，验证：销毁队列正确清空、（VK）验证层 0 error、
// 退出时分配器 live=0（退出日志）。
void RenderSystem::runChurnTest() {
    const char* e = std::getenv("RHI_CHURN");
    if (!e) return;
    const int rounds = std::max(1, std::atoi(e));
    if (!m_rhiDevice) return;
    std::vector<unsigned char> px(64 * 64 * 4, 128);
    for (int i = 0; i < rounds; ++i) {
        if (!m_rhiDevice->beginFrame()) break;
        std::vector<rhi::TextureHandle> texs;
        for (int k = 0; k < 8; ++k) {
            rhi::TextureDesc td;
            td.width = 64; td.height = 64;
            td.usage = rhi::TEX_SAMPLED;
            td.initialData = px.data();
            texs.push_back(m_rhiDevice->createTexture(td));
        }
        for (auto& t : texs) m_rhiDevice->destroyTexture(t);
        m_rhiDevice->endFrame();
        m_rhiDevice->present();   // C1a present 拆分后：acquire 的图像必须归还
    }
    m_rhiDevice->waitIdle();
    ENGINE_LOG_INFO("[CHURN] %d rounds x 8 textures created+destroyed", rounds);
}

void RenderSystem::update() {

}

void RenderSystem::BeginDraw() {

}

// ---- C1b：统一帧流程。宿主每帧调用一次 renderFrame() ----
//   beginFrame → RHI 场景（含阴影/拾取/叠加/UI）→ endFrame → present
// 零后端分支：GL 的 sRGB 等状态由 GLCommandList 在 Pass 边界处理。
void RenderSystem::renderFrame() {
    if (!m_rhiDevice) {
        // 设备创建失败的退化路径：宿主 UI 自行绘制
        if (m_overlayFn) m_overlayFn(nullptr);
        return;
    }
    // V3-0：beginFrame 失败（swapchain 过期/重建中）= 本帧跳过全部录制
    if (!m_rhiDevice->beginFrame()) return;

    // B4/D2 验证设施：渲染在帧内录制，回读推迟到下一帧（上一帧已提交）
    runRenderTargetReadbackTest();
    runPickPass();
    drawRhiSceneContents();

    m_rhiDevice->endFrame();
    m_rhiDevice->present();   // GL: 宿主 swapBuffers 回调 / VK: vkQueuePresentKHR

    // E3：剔除统计（每 120 帧一次，低频观测；RHI_CULL_STATS=1 才输出）
    {
        static int s_cullLogFrame = 0;
        if (mCurScene && std::getenv("RHI_CULL_STATS") && ++s_cullLogFrame % 120 == 0) {
            ENGINE_LOG_INFO("[CULL] culled=%u (frustumCull=%d)",
                            mCurScene->lastCulledCount(),
                            m_settings.frustumCull ? 1 : 0);
        }
    }
}

// 过渡别名：保留一个迭代（旧主循环入口），内部转调统一帧流程
void RenderSystem::draw() {
    renderFrame();
}

// 惰性创建附加 primary 列表（多线程录制拆批；GL 退化为返回主列表，不加入）
void RenderSystem::ensureSecondaryLists(int count) {
    while (static_cast<int>(m_secondaryLists.size()) < count) {
        rhi::IRHICommandList* l = m_rhiDevice->createCommandList();
        if (!l || l == m_rhiDevice->commandList()) break;
        m_secondaryLists.push_back(l);
    }
}

// Vulkan 场景帧内容（renderFrame 的 VK 分支；调用方已 beginFrame）
void RenderSystem::drawRhiSceneContents() {
    if (!m_rhiDevice || !mCurScene) return;
    auto* cmd = m_rhiDevice->commandList();
    if (!cmd) return;

    // V3-18：先画阴影深度图（独立 Pass），再画主场景
    recordShadowPass(cmd);

    rhi::RenderPassDesc rpClear;
    rpClear.clearColor = true;
    rpClear.clearDepth = true;
    // B3c：清屏色按作者 sRGB 值给出；sRGB 附件按线性值编码，故先转线性
    rpClear.clearColorValue[0] = color::srgbToLinear(0.2f);
    rpClear.clearColorValue[1] = color::srgbToLinear(0.3f);
    rpClear.clearColorValue[2] = color::srgbToLinear(0.3f);
    rpClear.clearColorValue[3] = 1.0f;
    rhi::RenderPassDesc rpLoad;   // 后续批次/overlay：不 clear，保留颜色+深度
    static bool emptyScene = (std::getenv("RHI_EMPTY") != nullptr);   // 调试开关：仅清屏+UI（VK 面板排查用）
    static bool once = false;
    if (!once) { once = true; ENGINE_LOG_ERROR("[SCENE] emptyScene=%d objs=%zu", emptyScene ? 1 : 0, mCurScene->GetObjs().size()); }
    if (!emptyScene) {
        if (auto cam = mCurScene->GetActiveCamera()) {
            auto vp = cam->GetVPMatrix();

            // C2：多视口（RHI_VIEWPORTS=2 左右分屏 / 4 四分屏）。
            // 同一相机同一场景，逐 tile 切 viewport/scissor 重绘；UI/overlay 在
            // tile 循环外按全目标绘制。tile 坐标约定 = 目标左上原点（与窗口一致）。
            static int multiView = [] {
                if (const char* e = std::getenv("RHI_VIEWPORTS")) {
                    return std::atoi(e);
                }
                return 1;
            }();
            const bool multi = multiView > 1;
            const auto fb = m_rhiDevice->framebufferSize();   // C1b：设备尺寸，不查窗口
            int fw = static_cast<int>(fb.width), fh = static_cast<int>(fb.height);

            // C2 收尾：多相机视口——tile 0 恒为主相机，tile i>0 依次用
            // Scene::addViewportCamera 加入的相机；无自定义相机时全部 tile
            // 用主相机（行为与旧版一致）
            const auto vpCams = mCurScene->getViewportCameras();

            auto drawSceneContent = [&](const Matrix& tileVP,
                                        const std::shared_ptr<Camera>& tileCam) {
                g_stats.on = (std::getenv("RHI_FRAME_STATS") != nullptr);
                const auto tCull0 = std::chrono::high_resolution_clock::now();
                // D3：透明排序 + E3：视锥剔除——按 tile 相机计算（多相机下
                // 各 tile 的可见集不同，剔除与排序必须逐 tile 做）
                const auto drawList = mCurScene->getSortedDrawList(tileCam->getPossition(),
                                                                   tileVP.buffer());
                const auto tCull1 = std::chrono::high_resolution_clock::now();
                static bool sortLogged = false;
                if (!sortLogged && std::getenv("RHI_ALPHA_DEMO")) {
                    sortLogged = true;
                    for (size_t i = 0; i < drawList.size(); ++i) {
                        auto model = std::dynamic_pointer_cast<Model>(drawList[i]);
                        if (model && model->getAlpha() < 0.999f) {
                            const glm::vec3 c = model->worldCenter();
                            ENGINE_LOG_INFO("[SORT] idx=%zu model id=%u alpha=%.2f center=(%.1f,%.1f,%.1f)",
                                            i, model->GetID(), model->getAlpha(), c.x, c.y, c.z);
                        }
                    }
                }
                // D3/D4 顺序契约：不透明 → 天空盒 → overlay/透明（视深排序）。
                // 透明与 overlay 不写深度，天空若在其后绘制会因深度测试覆盖
                // 它们的输出（透明被覆盖 = 混合结果丢失；overlay 被覆盖 = 全消失）。
                std::vector<std::shared_ptr<BaseDraw>> opaque, transparent;
                for (auto& obj : drawList) {
                    if (Scene::isTransparentDraw(obj) || obj->isOverlayDraw())
                        transparent.push_back(obj);
                    else
                        opaque.push_back(obj);
                }

                // 多线程拆批（方案 C：多个 primary 各自 begin/endRenderPass，深度 LOAD 连续）
                static const int mtThreads = [] {
                    const char* e = std::getenv("RHI_MT");
                    return e ? std::atoi(e) : 0;
                }();
                if (mtThreads > 1 && opaque.size() > 64
                    && m_rhiDevice->backend() == rhi::Backend::Vulkan && !multi) {
                    ensureSecondaryLists(mtThreads);
                    const size_t n = opaque.size();
                    const size_t perBatch = (n + mtThreads - 1) / mtThreads;
                    // 批 0（附加列表 0）：begin(clear) + 地面 + 批0 + end
                    {
                        rhi::IRHICommandList* list = m_secondaryLists[0];
                        list->beginRenderPass(rpClear);
                        recordGroundRHI(list, tileVP);
                        for (size_t i = 0; i < std::min(perBatch, n); ++i)
                            opaque[i]->draw(list, tileVP);
                        list->endRenderPass();
                    }
                    // 批 1..N-1（附加列表）：begin(load) + 批 + end（工作线程）
                    std::vector<std::thread> workers;
                    for (int b = 1; b < mtThreads; ++b) {
                        const size_t s = b * perBatch;
                        const size_t e = std::min(s + perBatch, n);
                        if (s >= e) continue;
                        rhi::IRHICommandList* list = m_secondaryLists[b];
                        workers.emplace_back([list, &opaque, s, e, tileVP, &rpLoad]() {
                            list->beginRenderPass(rpLoad);
                            for (size_t i = s; i < e; ++i) opaque[i]->draw(list, tileVP);
                            list->endRenderPass();
                        });
                    }
                    for (auto& t : workers) t.join();
                    // 主列表：begin(load) + 天空盒 + 透明 + end
                    cmd->beginRenderPass(rpLoad);
                    cmd->pushDebugLabel("RhiScene");
                    if (m_settings.drawSkyPass) {
                        if (auto sky = mCurScene->GetSkyBox())
                            sky->drawRHI(cmd, tileCam->GetViewMatrix(), tileCam->GetProjectMatrix());
                    }
                    for (auto& obj : transparent) obj->draw(cmd, tileVP);
                    cmd->endRenderPass();
                } else {
                    // 单线程：主列表 begin(clear) + 地面 + 全部 + 天空盒 + 透明 + end
                    cmd->beginRenderPass(rpClear);
                    cmd->pushDebugLabel("RhiScene");
                    recordGroundRHI(cmd, tileVP);
                    for (auto& obj : opaque) obj->draw(cmd, tileVP);
                    if (m_settings.drawSkyPass) {
                        if (auto sky = mCurScene->GetSkyBox())
                            sky->drawRHI(cmd, tileCam->GetViewMatrix(), tileCam->GetProjectMatrix());
                    }
                    for (auto& obj : transparent) obj->draw(cmd, tileVP);
                    cmd->endRenderPass();
                }
                const auto tRec1 = std::chrono::high_resolution_clock::now();
                g_stats.accum(std::chrono::duration<double, std::milli>(tCull1 - tCull0).count(),
                              std::chrono::duration<double, std::milli>(tRec1 - tCull1).count(),
                              drawList.size());
            };

            if (multi && fw > 0 && fh > 0) {
                const int cols = (multiView == 2) ? 2 : 2;   // 2=左右分屏，4=2×2
                const int rows = (multiView == 2) ? 1 : 2;
                const int tw = fw / cols, th = fh / rows;
                for (int r = 0; r < rows; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        const int tx = c * tw, ty = r * th;
                        // C2 收尾：每 tile 独立相机（tile 0 = 主相机；纵按相机
                        // 逻辑宽高比 = tile 尺寸）
                        std::shared_ptr<Camera> tileCam = cam;
                        const int idx = r * cols + c;
                        if (idx > 0 && !vpCams.empty()) {
                            tileCam = vpCams[(idx - 1) % vpCams.size()];
                        }
                        tileCam->setViewport(tw, th);
                        const Matrix tileVP = tileCam->GetVPMatrix();
                        // VK：负高度 viewport（B3a），锚点 y = tile 底（tileY + tileH）
                        cmd->setViewport(static_cast<float>(tx), static_cast<float>(ty + th),
                                         static_cast<float>(tw), -static_cast<float>(th));
                        cmd->setScissor(tx, ty, tw, th);
                        drawSceneContent(tileVP, tileCam);
                    }
                }
                // 恢复全目标（overlay/UI 按 swapchain 全尺寸绘制）
                cmd->setViewport(0, static_cast<float>(fh), static_cast<float>(fw), -static_cast<float>(fh));
                cmd->setScissor(0, 0, fw, fh);
            } else {
                drawSceneContent(vp, cam);
            }
        }
    }
    // RHI 验证叠加内容 + UI（独立 render pass，load 保留场景颜色与深度）
    cmd->beginRenderPass(rpLoad);
    if (m_settings.rhiSmokeTriangle) recordSmokeTriangle(cmd);
    if (m_settings.rhiTexQuad)       recordTexturedQuad(cmd);
    // UI 层叠加（ImGui Vulkan 等）。GL：关 sRGB 写编码，保持 ImGui 字节外观
    if (m_overlayFn) {
        cmd->setSrgbWrite(false);
        m_overlayFn(cmd);
    }
    cmd->endRenderPass();
}

// ---- B4 验证设施（RHI_RT_TEST=1）：离屏 RT 渲染 → 同步回读 ----
// 两阶段状态机：阶段 0 在帧内渲染冒烟三角形到 128x128 RT；
// 阶段 1（下一帧，VK 上一帧命令已提交完成）readbackTexture 回读，
// 打印中心/角落像素并与 GL 后端交叉比对。
// RT 颜色格式 = BGRA8_SRGB（与管线烘焙格式兼容，见 stage 0 注释），
// 双端存储/回读字节均为 sRGB 编码 + BGRA 序：
//   中心 ≈ <B>,<G>,<R>,255 = 213,141,122（顶点色线性插值后 sRGB 编码，60% 高处）
//   角落 ≈ 124,89,63,255（clear linear 0.05/0.1/0.2 编码）
void RenderSystem::runRenderTargetReadbackTest() {
    static bool enabled = (std::getenv("RHI_RT_TEST") != nullptr);
    static int stage = 0;
    if (!enabled || !m_rhiDevice) return;
    auto* cmd = m_rhiDevice->commandList();
    if (!cmd) return;

    if (stage == 0) {
        // sRGB 写编码由 GLCommandList 在 Pass 边界处理（VK 由附件格式驱动）
        rhi::RenderTargetDesc rtd;
        rtd.width  = 128;
        rtd.height = 128;
        // VK dynamic rendering 契约：管线创建时烘焙附件格式（PipelineRenderingCreateInfo
        // = swapchain 格式），RT 颜色/深度格式必须与管线兼容 → 用交换链同款格式。
        // （跨格式 RT 需要"管线格式集"，属 D5 显示模式阶段的管线变体工作）
        rtd.colorFormat = rhi::Format::BGRA8_SRGB;
        rtd.depthFormat = rhi::Format::Depth32F;
        rtd.samples     = m_rtMsaaSamples;   // RHI_RT_MSAA 时 >1
        // 深度附件必须存在：冒烟管线开深度测试，Vulkan 要求 rendering
        // 提供兼容的深度附件（无深度附件 + 深度测试管线 = MoltenVK abort）
        rtd.hasDepth    = true;
        m_rtTestTarget = m_rhiDevice->createRenderTarget(rtd);
        if (!m_rtTestTarget.valid()) {
            ENGINE_LOG_ERROR("[RTTEST] createRenderTarget failed");
            stage = 2;
            return;
        }
        rhi::RenderPassDesc rp;
        rp.renderTarget = m_rtTestTarget;
        rp.clearColor   = true;
        rp.clearDepth   = true;
        rp.clearColorValue[0] = 0.05f;   // 线性值直接写入 UNORM（RT 无 sRGB 编码）
        rp.clearColorValue[1] = 0.10f;
        rp.clearColorValue[2] = 0.20f;
        rp.clearColorValue[3] = 1.0f;
        cmd->beginRenderPass(rp);
        recordSmokeTriangleWith(cmd, m_rtMsaaSamples > 1 && m_smokePipelineMsaa.valid()
                                         ? m_smokePipelineMsaa : m_smokePipeline);
        cmd->endRenderPass();
        stage = 1;
        return;
    }
    if (stage != 1) return;
    stage = 2;

    std::vector<unsigned char> px(128 * 128 * 4);
    const rhi::TextureHandle colorTex = m_rhiDevice->renderTargetColorTexture(m_rtTestTarget);
    const bool ok = m_rhiDevice->readbackTexture(colorTex, px.data(), px.size());
    if (!ok) {
        ENGINE_LOG_ERROR("[RTTEST] readbackTexture failed");
    } else {
        // 冒烟三角形质心 NDC(0,-0.2) → 60% 高处。行序契约差异（B4 已知）：
        // VK 图像 row 0 = 画面顶部；GL 附件回读 row 0 = 画面底部（GL y-up 纹理
        // 约定）→ 同一画面点在两端落在镜像行上，取样行按后端镜像取。
        const size_t cx  = 128 / 2;
        const size_t cy  = 128 * 3 / 5;
        const size_t row = (m_rhiDevice->backend() == rhi::Backend::Vulkan) ? (127 - cy) : cy;
        const size_t off    = (row * 128 + cx) * 4;
        const size_t corner = 0;
        ENGINE_LOG_INFO("[RTTEST] center bytes = %d,%d,%d,%d (BGRA, expect ~213,141,122,255)",
                        px[off], px[off+1], px[off+2], px[off+3]);
        ENGINE_LOG_INFO("[RTTEST] corner bytes = %d,%d,%d,%d (BGRA, expect ~124,89,63,255)",
                        px[corner], px[corner+1], px[corner+2], px[corner+3]);
        // PPM 导出（目视检查；与 RHI_DUMP 同目录约定）
        std::error_code ec;
        auto tmpDir = std::filesystem::temp_directory_path(ec);
        if (ec) tmpDir = ".";
        const char* tag = (m_rhiDevice->backend() == rhi::Backend::Vulkan) ? "vk" : "gl";
        FILE* fp = engine::fs::openWrite(tmpDir / (std::string("frame_rt_") + tag + ".ppm"));
        if (fp) {
            std::fprintf(fp, "P6\n128 128\n255\n");
            for (int y = 0; y < 128; ++y) {
                for (int x = 0; x < 128; ++x) {
                    const size_t i = (static_cast<size_t>(y) * 128 + x) * 4;
                    std::fwrite(&px[i], 1, 3, fp);
                }
            }
            std::fclose(fp);
            ENGINE_LOG_INFO("[RTTEST] frame written: frame_rt_%s.ppm", tag);
        }
    }
    m_rhiDevice->destroyRenderTarget(m_rtTestTarget);
    m_rtTestTarget = rhi::RenderTargetHandle{};
    ENGINE_LOG_INFO("[RTTEST] done");
}

// ---- D2: RHI 拾取（ID 渲染 + 回读）----
// pick RT = BGRA8_SRGB（与管线烘焙格式兼容，同 B4 约束）；ID 经 push constant
// uPickColor 直出，sRGB 编码往返在 8bit 精度下可逆（byte→linear→byte 恒等），
// alpha 通道不编码。requestPick 记录点击点：第 N 帧（runPickPass stage 0）渲染
// ID 图，第 N+1 帧（stage 1）回读 1px 解码对象 ID。
// 行序约定（B4 已知差异）：VK row0 = 画面顶（鼠标 y 直接可用）；
// GL 附件回读 row0 = 画面底 → y 镜像。
void RenderSystem::ensurePickResources() {
    if (m_pickPipeline.valid() && m_pickRT.valid()) return;
    auto* device = m_rhiDevice;
    if (!device) return;

    rhi::GraphicsPipelineDesc pd;
    pd.shaders = {
        { rhi::ShaderStage::Vertex,   "vert_pick_rhi" },
        { rhi::ShaderStage::Fragment, "frag_pick_rhi" },
    };
    // 顶点布局与模型绘制管线一致（复用同一套 VB/IB/实例缓冲）
    pd.vertexLayout.stride = 8 * sizeof(float);          // pos3+normal3+uv2
    pd.vertexLayout.instanceStride = sizeof(Matrix);     // 实例矩阵 mat4
    pd.vertexLayout.attributes = {
        { 0, rhi::VertexFormat::Float3, 0,  false },
        { 1, rhi::VertexFormat::Float3, 12, false },
        { 2, rhi::VertexFormat::Float2, 24, false },
        { 3, rhi::VertexFormat::Float4, 0,  true },
        { 4, rhi::VertexFormat::Float4, 16, true },
        { 5, rhi::VertexFormat::Float4, 32, true },
        { 6, rhi::VertexFormat::Float4, 48, true },
    };
    pd.primitive = rhi::PrimitiveType::Triangles;
    pd.cull      = rhi::CullMode::Back;
    pd.depth     = { true, true };
    m_pickPipeline = device->createPipeline(pd);

    // RT 尺寸取设备帧缓冲尺寸（拾取点为 framebuffer 像素坐标）
    const auto fb = device->framebufferSize();
    int fbW = static_cast<int>(fb.width);
    int fbH = static_cast<int>(fb.height);
    if (fbW <= 0 || fbH <= 0) return;

    rhi::RenderTargetDesc rtd;
    rtd.width  = static_cast<uint32_t>(fbW);
    rtd.height = static_cast<uint32_t>(fbH);
    rtd.colorFormat = rhi::Format::BGRA8_SRGB;   // 与管线烘焙格式兼容（B4 契约）
    rtd.depthFormat = rhi::Format::Depth32F;
    rtd.hasDepth    = true;
    m_pickRT = device->createRenderTarget(rtd);
    if (m_pickRT.valid()) {
        ENGINE_LOG_INFO("[PICK] pick RT %dx%d + pipeline ready", fbW, fbH);
    }
}

// V3-18：光空间矩阵 → Vulkan 深度约定
// 与 VkCommandList 的 Reversed-Z 修正同式：z' = 0.5*(w - z)，近平面 → 1、远平面 → 0。
// 阴影贴图按这个矩阵写入，采样比较时按同一约定（存得越大越近）。
static Matrix correctedLightVP() {
    glm::mat4 m = Matrix::toMatrix(Scene::GetLightVPMatrix());
    for (int c = 0; c < 4; ++c) {
        float* z = &m[c][2];
        const float w = m[c][3];
        *z = 0.5f * (w - *z);
    }
    return Matrix::toMatrix(m);
}

void RenderSystem::ensureShadowResources() {
    auto* device = m_rhiDevice;
    if (!device) return;
    if (m_shadowInitTried) return;   // 建过一次就不再重试（着色器缺失属于构建期问题）
    m_shadowInitTried = true;

    rhi::GraphicsPipelineDesc sd;
    sd.shaders = {
        { rhi::ShaderStage::Vertex,   "vert_shadow_rhi" },
        { rhi::ShaderStage::Fragment, "frag_shadow_rhi" },
    };
    sd.vertexLayout.stride = 8 * sizeof(float);
    sd.vertexLayout.instanceStride = sizeof(Matrix);
    sd.vertexLayout.attributes = {
        { 0, rhi::VertexFormat::Float3, 0,  false },
        { 1, rhi::VertexFormat::Float3, 12, false },
        { 2, rhi::VertexFormat::Float2, 24, false },
        { 3, rhi::VertexFormat::Float4, 0,  true },
        { 4, rhi::VertexFormat::Float4, 16, true },
        { 5, rhi::VertexFormat::Float4, 32, true },
        { 6, rhi::VertexFormat::Float4, 48, true },
    };
    sd.primitive = rhi::PrimitiveType::Triangles;
    sd.cull      = rhi::CullMode::Back;
    sd.depth     = { true, true };
    sd.attachment.colorCount  = 0;                 // 纯深度管线
    sd.attachment.depthFormat = rhi::Format::Depth32F;
    m_shadowPipeline = device->createPipeline(sd);

    rhi::RenderTargetDesc rtd;
    rtd.width  = 1024;
    rtd.height = 1024;
    rtd.hasColor = false;                          // 纯深度目标
    rtd.hasDepth = true;
    rtd.depthFormat = rhi::Format::Depth32F;
    m_shadowRT = device->createRenderTarget(rtd);
    if (!m_shadowRT.valid()) {
        ENGINE_LOG_ERROR("[SHADOW] shadow target create failed");
    } else {
        m_shadowTex = device->renderTargetDepthTexture(m_shadowRT);
        ENGINE_LOG_INFO("[SHADOW] 1024x1024 depth target + depth-only pipeline ready");
    }

    // V3-20：阴影接收面——20x20 地面（世界 y=0），棋盘格 + PCF
    {
        const float half = 20.0f;
        // 相机在 y=0 水平看，地面必须低于相机才可见（否则与视线共面）
        const float gy = -2.0f;
        const float verts[] = {
            -half, gy, -half,   half, gy, -half,   half, gy,  half,
            -half, gy, -half,   half, gy,  half,  -half, gy,  half,
        };
        rhi::BufferDesc vbd;
        vbd.size = sizeof(verts);
        vbd.usage = rhi::BUF_VERTEX;
        vbd.initialData = verts;
        m_groundVB = device->createBuffer(vbd);

        rhi::GraphicsPipelineDesc gd;
        gd.shaders = {
            { rhi::ShaderStage::Vertex,   "vert_ground_rhi" },
            { rhi::ShaderStage::Fragment, "frag_ground_rhi" },
        };
        gd.vertexLayout.stride = 3 * sizeof(float);
        gd.vertexLayout.attributes = { { 0, rhi::VertexFormat::Float3, 0, false } };
        gd.primitive = rhi::PrimitiveType::Triangles;
        gd.cull      = rhi::CullMode::None;
        gd.depth     = { true, true };
        m_groundPipeline = device->createPipeline(gd);
        if (!m_groundPipeline.valid()) {
            ENGINE_LOG_ERROR("[SHADOW] ground pipeline create failed");
        }
    }
}

// V3-20：地面绘制（阴影接收面）。放在主场景 Pass 内，先于其它不透明物体。
void RenderSystem::recordGroundRHI(rhi::IRHICommandList* cmd, const Matrix& viewProj) {
    if (!m_groundPipeline.valid() || !m_groundVB.valid() || !m_shadowTex.valid()) return;
    if (!m_settings.drawShadowPass) return;

    cmd->pushDebugLabel("ShadowGround");
    cmd->bindPipeline(m_groundPipeline);
    glm::mat4 vp = Matrix::toMatrix(viewProj);
    cmd->setUniform("uViewProj", rhi::UniformValue::fromMat4(glm::value_ptr(vp)));
    glm::mat4 lm = Matrix::toMatrix(correctedLightVP());
    cmd->setUniform("uLightMat", rhi::UniformValue::fromMat4(glm::value_ptr(lm)));
    cmd->bindTexture(m_shadowTex, 4, "uShadowMap");
    cmd->bindVertexBuffer(m_groundVB);
    cmd->draw(6);
    cmd->popDebugLabel();
}

// V3-19：主场景绘制用的光空间矩阵（与阴影 Pass 完全一致）
glm::vec3 RenderSystem::activeCameraPos() const {
    if (mCurScene) {
        if (auto cam = mCurScene->GetActiveCamera()) return cam->getPossition();
    }
    return Camera::GetCamera().getPossition();
}

Matrix RenderSystem::shadowLightVP() const {
    Matrix m = correctedLightVP();
    return m;
}

void RenderSystem::recordShadowPass(rhi::IRHICommandList* cmd) {
    if (!m_settings.drawShadowPass || !cmd || !mCurScene) return;
    ensureShadowResources();
    if (!m_shadowRT.valid() || !m_shadowPipeline.valid()) return;

    rhi::RenderPassDesc sp;
    sp.renderTarget = m_shadowRT;
    sp.clearColor   = false;   // 纯深度目标无颜色附件
    sp.clearDepth   = true;
    cmd->beginRenderPass(sp);
    cmd->pushDebugLabel("Shadow");
    const Matrix lightVP = correctedLightVP();
    uint32_t drawn = 0;
    for (auto& obj : mCurScene->GetObjs()) {
        if (auto model = std::dynamic_pointer_cast<Model>(obj)) {
            if (model->drawShadowRHI(cmd, m_shadowPipeline, lightVP)) ++drawn;
        }
    }
    static bool logged = false;
    if (!logged && drawn > 0) {
        logged = true;
        ENGINE_LOG_INFO("[SHADOW] shadow pass recording %u model(s) into 1024x1024 depth map", drawn);
    }
    cmd->popDebugLabel();
    cmd->endRenderPass();
}

void RenderSystem::requestPick(double x, double y, bool isHover) {
    if (!m_rhiDevice) return;
    // 窗口逻辑坐标 → framebuffer 像素坐标（Retina 下两者不同；比例来自
    // InitDesc 的 windowWidth/windowHeight，C1b 起引擎不查询窗口系统）
    if (m_windowW <= 0 || m_windowH <= 0) return;
    const double sx = static_cast<double>(m_frameW) / m_windowW;
    const double sy = static_cast<double>(m_frameH) / m_windowH;
    m_pickReq.pending  = true;
    m_pickReq.rendered = false;
    m_pickReq.isHover  = isHover;
    m_pickReq.x = static_cast<uint32_t>(x * sx);
    m_pickReq.y = static_cast<uint32_t>(y * sy);
}

void RenderSystem::runPickPass() {
    static bool selfTest = (std::getenv("RHI_PICK_TEST") != nullptr);
    static int  selfTestFrame = 0;
    if (!m_rhiDevice) return;
    auto* cmd = m_rhiDevice->commandList();
    if (!cmd) return;

    // RHI_PICK_TEST：第 40 帧自动在屏幕中心拾取一次（场景稳定后）。
    // requestPick 入参 = 窗口逻辑坐标（内部按 InitDesc 比例换算 framebuffer 像素）
    if (selfTest && !m_pickReq.pending && !m_pickReq.rendered && ++selfTestFrame == 40) {
        requestPick(m_windowW / 2.0, m_windowH * 0.55);   // 鸭子群中心附近
    }
    // D8 悬停拾取（节流）：光标移动 ≥ 3px、冷却 ≥ 6 帧、无待处理拾取时请求
    if (m_settings.hoverEnabled && !m_pickReq.pending && !m_pickReq.rendered
        && m_hoverLastX > -1e8 && --m_hoverCooldown <= 0) {
        static double s_lastHoverX = -1e9, s_lastHoverY = -1e9;
        const double dx = m_hoverLastX - s_lastHoverX;
        const double dy = m_hoverLastY - s_lastHoverY;
        if (dx * dx + dy * dy > 9.0) {
            s_lastHoverX = m_hoverLastX;
            s_lastHoverY = m_hoverLastY;
            m_hoverCooldown = 6;
            requestPick(m_hoverLastX, m_hoverLastY, /*isHover=*/true);
        }
    }
    if (!m_pickReq.pending && !m_pickReq.rendered) return;

    const Matrix mtxVP = mCurScene->GetActiveCamera()
        ? mCurScene->GetActiveCamera()->GetVPMatrix() : Matrix();

    if (m_pickReq.pending) {
        // ---- stage 0: 渲染 ID 图 ----
        ensurePickResources();
        if (!m_pickPipeline.valid() || !m_pickRT.valid()) {
            m_pickReq.pending = false;
            return;
        }
        rhi::RenderPassDesc rp;
        rp.renderTarget = m_pickRT;
        rp.clearColor   = true;
        rp.clearDepth   = true;
        rp.clearColorValue[0] = 0.0f;   // 清屏 = 无命中（a=0 与对象 a=1 区分）
        rp.clearColorValue[1] = 0.0f;
        rp.clearColorValue[2] = 0.0f;
        rp.clearColorValue[3] = 0.0f;
        cmd->beginRenderPass(rp);
        for (auto& obj : mCurScene->GetObjs()) {
            auto model = std::dynamic_pointer_cast<Model>(obj);
            if (model) {
                model->drawPickRHI(cmd, m_pickPipeline, mtxVP);
            }
        }
        cmd->endRenderPass();
        m_pickReq.pending  = false;
        m_pickReq.rendered = true;   // VK：下一帧（本帧命令已提交）再回读
        return;
    }

    // ---- stage 1: 回读 + 解码 ----
    if (!m_pickReq.rendered) return;
    m_pickReq.rendered = false;

    // RHI_PICK_TEST：全图扫描验证 渲染→回读→解码 全链路（收集命中 ID + 位置）
    if (selfTest) {
        auto colorTex = m_rhiDevice->renderTargetColorTexture(m_pickRT);
        const auto fb = m_rhiDevice->framebufferSize();
        const int fbW = static_cast<int>(fb.width), fbH = static_cast<int>(fb.height);
        std::vector<unsigned char> img(static_cast<size_t>(fbW) * fbH * 4);
        rhi::ReadbackDesc rbFull;
        if (!m_rhiDevice->readbackTexture(colorTex, img.data(), img.size(), rbFull)) {
            ENGINE_LOG_ERROR("[PICK] full readback failed");
            return;
        }
        std::map<uint16_t, size_t> idCount;
        size_t firstHit = SIZE_MAX;
        for (size_t i = 0; i < img.size(); i += 4) {
            if (img[i + 3] >= 128) {
                // 回读字节序 = BGRA（双端一致，B4 已验证）：R=img[i+2] G=img[i+1]
                const uint16_t id = static_cast<uint16_t>(img[i + 2] | (img[i + 1] << 8));
                idCount[id]++;
                if (firstHit == SIZE_MAX) firstHit = i / 4;
            }
        }
        ENGINE_LOG_INFO("[PICK][selftest] distinct IDs=%zu, first hit pixel index=%zu",
                        idCount.size(), firstHit == SIZE_MAX ? SIZE_MAX : firstHit);
        for (const auto& kv : idCount) {
            ENGINE_LOG_INFO("[PICK][selftest] id=%u pixels=%zu", kv.first, kv.second);
        }
        // D8 门禁：自测自动选中命中集合中最小 ID 的对象，高亮帧可经
        // RHI_DUMP 目视；点击空白不改变选中
        if (!idCount.empty()) {
            m_settings.selectedId = idCount.begin()->first;
            ENGINE_LOG_INFO("[HIGHLIGHT] auto-selected id=%u (strength=%.2f)",
                            m_settings.selectedId, m_settings.highlightStrength);
        }
        // 调试导出：ID 图原样落盘（PPM）
        {
            std::error_code ec;
            auto tmpDir = std::filesystem::temp_directory_path(ec);
            if (ec) tmpDir = ".";
            FILE* fp = engine::fs::openWrite(tmpDir / "pick_debug.ppm");
            if (fp) {
                std::fprintf(fp, "P6\n%d %d\n255\n", fbW, fbH);
                for (size_t i = 0; i < img.size(); i += 4) {
                    std::fwrite(&img[i], 1, 3, fp);
                }
                std::fclose(fp);
                ENGINE_LOG_INFO("[PICK][selftest] pick_debug.ppm written");
            }
        }
    }

    auto colorTex = m_rhiDevice->renderTargetColorTexture(m_pickRT);
    unsigned char px[4] = {0, 0, 0, 0};
    rhi::ReadbackDesc rb;
    // 行序镜像：VK row0 = 画面顶（鼠标原点），GL row0 = 画面底
    const auto fb = m_rhiDevice->framebufferSize();
    const int fbH = static_cast<int>(fb.height);
    rb.x = m_pickReq.x;
    rb.y = (m_rhiDevice->backend() == rhi::Backend::Vulkan)
               ? m_pickReq.y
               : (fbH > 0 ? static_cast<uint32_t>(fbH) - 1 - m_pickReq.y : m_pickReq.y);
    rb.width = 1;
    rb.height = 1;
    if (!m_rhiDevice->readbackTexture(colorTex, px, sizeof(px), rb)) {
        ENGINE_LOG_ERROR("[PICK] readback failed at (%u,%u)", rb.x, rb.y);
        return;
    }
    if (px[3] < 128) {
        if (!m_pickReq.isHover) {
            ENGINE_LOG_INFO("[PICK] no hit at (%u,%u)", rb.x, rb.y);
        }
        if (m_pickReq.isHover) {
            m_settings.hoverId = 0;   // 悬停落空 → 清除提示
        }
        return;
    }
    // BGRA 序：R=px[2]（ID 低字节） G=px[1]（ID 高字节）
    const uint16_t hitId = static_cast<uint16_t>(px[2] | (px[1] << 8));
    if (m_pickReq.isHover) {
        m_settings.hoverId = hitId;   // 悬停命中 → 提示色（不改变选中）
        return;
    }
    // 解析对象：遍历场景 Model，匹配 GetID()；命中即选中（D8 高亮）
    for (auto& obj : mCurScene->GetObjs()) {
        auto model = std::dynamic_pointer_cast<Model>(obj);
        if (model && model->GetID() == hitId) {
            ENGINE_LOG_INFO("[PICK] HIT id=%u (Model) -> selected", hitId);
            m_settings.selectedId = hitId;   // D8：选中高亮
            return;
        }
    }
    ENGINE_LOG_INFO("[PICK] HIT id=%u (unknown object)", hitId);
}

void RenderSystem::createTexturedQuad() {
    if (!m_rhiDevice) return;

    rhi::GraphicsPipelineDesc pipeDesc;
    pipeDesc.shaders = {
        { rhi::ShaderStage::Vertex,   "vert_rhi_quad" },
        { rhi::ShaderStage::Fragment, "frag_rhi_quad" },
    };
    // pos(3) + uv(2)，stride 20 字节
    struct QuadVertex { float pos[3]; float uv[2]; };
    pipeDesc.vertexLayout.stride = sizeof(QuadVertex);
    pipeDesc.vertexLayout.attributes = {
        { 0, rhi::VertexFormat::Float3, offsetof(QuadVertex, pos) },
        { 1, rhi::VertexFormat::Float2, offsetof(QuadVertex, uv) },
    };
    pipeDesc.primitive = rhi::PrimitiveType::Triangles;
    pipeDesc.cull      = rhi::CullMode::None;
    pipeDesc.depth     = { false, false };
    pipeDesc.blend     = { false };
    m_quadPipeline = m_rhiDevice->createPipeline(pipeDesc);

    static const QuadVertex kQuadVerts[4] = {
        {{-0.5f, -0.5f, 0.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f, 0.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f, 0.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, 0.0f}, {0.0f, 1.0f}},
    };
    static const unsigned int kQuadIndices[6] = {0, 1, 2, 0, 2, 3};

    rhi::BufferDesc vbDesc;
    vbDesc.size = sizeof(kQuadVerts);
    vbDesc.usage = rhi::BUF_VERTEX;
    vbDesc.initialData = kQuadVerts;
    m_quadVB = m_rhiDevice->createBuffer(vbDesc);

    rhi::BufferDesc ibDesc;
    ibDesc.size = sizeof(kQuadIndices);
    ibDesc.usage = rhi::BUF_INDEX;
    ibDesc.initialData = kQuadIndices;
    m_quadIB = m_rhiDevice->createBuffer(ibDesc);

    // 1x1 紫色纹理：采样路径验证用（回读中心像素应精确等于此颜色）
    uint8_t purple[4] = {180, 60, 220, 255};
    rhi::TextureDesc texDesc;
    texDesc.width = 1; texDesc.height = 1;
    texDesc.usage = rhi::TEX_SAMPLED;
    // B3c：紫色是颜色值；sRGB 解码 + sRGB 交换链编码 → 回读仍为原字节
    texDesc.format = rhi::Format::RGBA8_SRGB;
    texDesc.initialData = purple;
    m_quadTex = m_rhiDevice->createTexture(texDesc);

    // 四边形专属 uniform 块：VK 后端 bindTexture 会写入“当前块”的描述符集，
    // 若沿用上一个模型的块会污染其纹理槽（描述符写入不按命令顺序生效）
    m_quadUB = m_rhiDevice->createUniformBlock(256);
}

void RenderSystem::recordTexturedQuad(rhi::IRHICommandList* cmd) {
    if (!cmd || !m_quadPipeline.valid() || !m_quadVB.valid() || !m_quadIB.valid() || !m_quadTex.valid()) return;

    cmd->pushDebugLabel("RHITexQuad");
    cmd->bindPipeline(m_quadPipeline);
    cmd->setUniform("uMatrixMVP", rhi::UniformValue::fromMat4(kIdentityMat4));
    cmd->setUniform("uTex", rhi::UniformValue::fromInt(0));
    // 切到四边形专属 uniform 块后再绑纹理：bindTexture 写入"当前块"的
    // 描述符集（VK），沿用模型块会污染模型纹理槽。GL 按名直传，
    // 不存在的 uniform 被驱动忽略，两端同一调用。
    if (!m_quadUB.valid()) return;
    static const unsigned char zeroBlock[256] = {};
    cmd->setDrawUniforms(m_quadUB, zeroBlock, sizeof(zeroBlock));
    cmd->bindTexture(m_quadTex, 0, "uTex");
    cmd->bindVertexBuffer(m_quadVB);
    cmd->bindIndexBuffer(m_quadIB);
    cmd->drawIndexed(6);
    cmd->popDebugLabel();
}


void RenderSystem::recordSmokeTriangle(rhi::IRHICommandList* cmd) {
    recordSmokeTriangleWith(cmd, m_smokePipeline);
}

void RenderSystem::recordSmokeTriangleWith(rhi::IRHICommandList* cmd, rhi::PipelineHandle pipeline) {
    if (!cmd || !pipeline.valid() || !m_smokeVB.valid()) return;

    cmd->pushDebugLabel("RHISmokeTriangle");
    cmd->bindPipeline(pipeline);
    cmd->setUniform("uMatrixMVP", rhi::UniformValue::fromMat4(kIdentityMat4));
    cmd->bindVertexBuffer(m_smokeVB);
    if (m_smokeIB.valid()) {
        cmd->bindIndexBuffer(m_smokeIB);
        cmd->drawIndexed(3);
    } else {
        cmd->draw(3);
    }
    cmd->popDebugLabel();
}

void RenderSystem::preloadTextures(const std::string& dirPath) {
    // A4: dirPath 按 UTF-8 解释（Windows 宽字符目录遍历）
    loadTexture(engine::fs::pathFromUtf8(dirPath));
}

void RenderSystem::loadTexture(const std::filesystem::path& dirPath) {
    for (const auto& entry : std::filesystem::directory_iterator(dirPath)) {
        if (entry.is_directory()) {
            loadTexture(entry.path());
        }
        else if (entry.is_regular_file()) {
            auto extension = entry.path().extension().string();
            if (extension == ".png" || extension == ".jpg" || extension == ".tga") {
                // A4: 资源键与打开路径统一为 UTF-8
                auto filename = engine::fs::utf8FromPath(entry.path());
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
        ENGINE_LOG_INFO("Click detected at (%.1f, %.1f)", x, y);
        requestPick(x, y);   // D2: RHI 拾取（两端同一 ID 图路径）
    }
}

void RenderSystem::onMouseRightDown(double x, double y) {
}

void RenderSystem::onMouseRightUp(double x, double y) {
}

void RenderSystem::onMouseMiddleDown(double x, double y) {
}

void RenderSystem::onMouseMove(double x, double y) {
    // D8 悬停：光标移动 ≥ 3px 记为悬停候选（每帧节流判断；拖拽中不悬停拾取）
    m_hoverLastX = x;
    m_hoverLastY = y;
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
    if (m_rhiDevice) {
        m_rhiDevice->onResize(x, y);   // Vulkan: swapchain 重建
    }
    
    auto camera = mCurScene->GetActiveCamera();
    camera->setViewport(x, y);
}
