#include "log/LogMacros.h"
#include "RenderSystem.h"
#include "render/scene.h"
#include "object/Sky.h"
#include "model/model.h"
#include "pass/SkyPass.h"
#include "pass/ShadowPass.h"
#include "pass/NormalPass.h"
#include "pass/PickPass.h"
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
} // namespace

RenderSystem& RenderSystem::getInstance() {
	static RenderSystem instance;
	return instance;
}

RenderSystem::RenderSystem() {
    mCurScene = std::make_shared<Scene>();
    mRenderContent = std::make_shared<RenderContext>();
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
    // 依赖 GL 上下文的对象（Pass 的 FBO、字体纹理等）在此释放
    m_vec_renderpass.clear();
    m_fontMng.reset();
    mCurScene.reset();
    mRenderContent.reset();
    m_rhiDevice = nullptr;   // 只清借用指针，不销毁设备
}

bool RenderSystem::init(const InitDesc& desc, std::string* outError) {
    if (!desc.device) {
        if (outError) *outError = "InitDesc.device is null";
        return false;
    }
    m_rhiDevice = desc.device;
    _backend    = desc.device->backend();
    m_frameW  = desc.frameWidth;
    m_frameH  = desc.frameHeight;
    m_windowW = desc.windowWidth  ? desc.windowWidth  : desc.frameWidth;
    m_windowH = desc.windowHeight ? desc.windowHeight : desc.frameHeight;
    ENGINE_LOG_INFO("[RHI] backend = %s (device provided by host)",
                    _backend == rhi::Backend::Vulkan ? "Vulkan" : "OpenGL");

    // ������ɫ��
    // A4: rootPath 约定为 UTF-8；Windows 上 path(std::string) 按 ACP 解释会错，
    // 统一走 pathFromUtf8（引擎内路径字符串 = UTF-8）
    auto shaderPath = engine::fs::pathFromUtf8(desc.assetRoot) / "res" / "shaders";
    // 注意：GL 着色器缓存仅在 OpenGL 后端初始化（Vulkan 模式无 GL 上下文，
    // glad 函数指针为空，任何 glCreate* 调用都会跳空地址）
    if (_backend == rhi::Backend::OpenGL) {
        ShaderCache::GetInstance().init(engine::fs::utf8FromPath(shaderPath) + std::string(1, std::filesystem::path::preferred_separator));
    }

	// ��ʼ������
    mCurScene->init();
    
    // 创建帧级 Pass（GL 专属实现：构造函数里就有 GL 调用如 FBO 创建，
    // 因此 Vulkan 模式下不创建——M4 中 Pass 逐步迁移到 RHI 后解除此限制）
    if (_backend == rhi::Backend::OpenGL) {
        m_vec_renderpass.push_back(std::make_shared<CShadowPass>(mRenderContent));
        m_vec_renderpass.push_back(std::make_shared<CNormalPass>(mRenderContent));
        m_vec_renderpass.push_back(std::make_shared<CSkyPass>(mRenderContent));
        m_vec_renderpass.push_back(std::make_shared<CPickPass>(mRenderContent));
    }

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
//   beginFrame → (GL: 遗留 Pass + RHI 叠加 / VK: RHI 场景) → overlayFn → endFrame → present
// GL/VK 主循环零后端分支（ImGui backend 初始化除外）；UI 合成点见 setOverlayRenderer。
void RenderSystem::renderFrame() {
    if (!m_rhiDevice) {
        // 设备创建失败的退化路径：遗留 GL Pass 照跑，宿主 UI 自行绘制
        renderScenePasses();
        if (m_overlayFn) m_overlayFn(nullptr);
        return;
    }
    // V3-0：beginFrame 失败（swapchain 过期/重建中）= 本帧跳过全部录制
    if (!m_rhiDevice->beginFrame()) return;

    if (_backend == rhi::Backend::Vulkan) {
        // B4/D2 验证设施：渲染在帧内录制，回读推迟到下一帧（上一帧已提交）
        runRenderTargetReadbackTest();
        runPickPass();
        drawRhiSceneContents();
    } else {
        // GL：线性工作流。默认帧缓冲以 GLFW_SRGB_CAPABLE 创建，写回时由硬件编码
        glEnable(GL_FRAMEBUFFER_SRGB);
        renderScenePasses();
        drawSceneOverlays();            // RHI 叠加内容（并入主帧，不再自建 frame）
        selfTestAB();                   // M4.2 验证设施（GL 即时语义）
        runRenderTargetReadbackTest();
        runPickPass();
        // UI 合成点：ImGui 等 UI 保持既有 gamma 空间外观（SRGB 编码关闭）
        glDisable(GL_FRAMEBUFFER_SRGB);
        if (m_overlayFn) m_overlayFn(m_rhiDevice->commandList());
    }

    m_rhiDevice->endFrame();
    m_rhiDevice->present();   // GL: 宿主 swapBuffers 回调 / VK: vkQueuePresentKHR
}

// 过渡别名：保留一个迭代（旧主循环入口），内部转调统一帧流程
void RenderSystem::draw() {
    renderFrame();
}

// Vulkan 场景帧内容（renderFrame 的 VK 分支；调用方已 beginFrame）
void RenderSystem::drawRhiSceneContents() {
    if (!m_rhiDevice || !mCurScene) return;
    auto* cmd = m_rhiDevice->commandList();
    if (!cmd) return;
    rhi::RenderPassDesc rp;
    rp.clearColor = true;
    rp.clearDepth = true;
    // B3c：清屏色按作者 sRGB 值给出；sRGB 附件按线性值编码，故先转线性
    rp.clearColorValue[0] = color::srgbToLinear(0.2f);
    rp.clearColorValue[1] = color::srgbToLinear(0.3f);
    rp.clearColorValue[2] = color::srgbToLinear(0.3f);
    rp.clearColorValue[3] = 1.0f;
    cmd->beginRenderPass(rp);
    cmd->pushDebugLabel("RhiScene");
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

            // D3：透明排序绘制列表（两端共用同一排序；每帧算一次）
            const auto drawList = mCurScene->getSortedDrawList(cam->getPossition());
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

            auto drawSceneContent = [&]() {
                // D3/D4 顺序契约：不透明 → 天空盒 → overlay/透明（视深排序）。
                // 透明与 overlay 不写深度，天空若在其后绘制会因深度测试覆盖
                // 它们的输出（透明被覆盖 = 混合结果丢失；overlay 被覆盖 = 全消失）。
                for (auto& obj : drawList) {
                    if (obj->getAlpha() < 0.999f || obj->isOverlayDraw()) continue;
                    obj->draw(vp);
                }
                // 天空盒（RHI 路径，M4.3；调试面板开关生效）
                if (m_settings.drawSkyPass) {
                    if (auto sky = mCurScene->GetSkyBox()) {
                        sky->drawRHI(cmd, cam->GetViewMatrix(), cam->GetProjectMatrix());
                    }
                }
                for (auto& obj : drawList) {
                    if (obj->getAlpha() < 0.999f || obj->isOverlayDraw()) {
                        obj->draw(vp);
                    }
                }
            };

            if (multi && fw > 0 && fh > 0) {
                const int cols = (multiView == 2) ? 2 : 2;   // 2=左右分屏，4=2×2
                const int rows = (multiView == 2) ? 1 : 2;
                const int tw = fw / cols, th = fh / rows;
                for (int r = 0; r < rows; ++r) {
                    for (int c = 0; c < cols; ++c) {
                        const int tx = c * tw, ty = r * th;
                        // VK：负高度 viewport（B3a），锚点 y = tile 底（tileY + tileH）
                        cmd->setViewport(static_cast<float>(tx), static_cast<float>(ty + th),
                                         static_cast<float>(tw), -static_cast<float>(th));
                        cmd->setScissor(tx, ty, tw, th);
                        drawSceneContent();
                    }
                }
                // 恢复全目标（overlay/UI 按 swapchain 全尺寸绘制）
                cmd->setViewport(0, static_cast<float>(fh), static_cast<float>(fw), -static_cast<float>(fh));
                cmd->setScissor(0, 0, fw, fh);
            } else {
                drawSceneContent();
            }
        }
    }
    // RHI 验证叠加内容（调试面板 / RHI_SMOKE / RHI_TEXQUAD 开关）
    // 在同一个 render pass 内录制，不再自建 frame/pass
    if (m_settings.rhiSmokeTriangle) recordSmokeTriangle(cmd);
    if (m_settings.rhiTexQuad)       recordTexturedQuad(cmd);
    // UI 层叠加（ImGui Vulkan 等）在 render pass 内渲染
    if (m_overlayFn) {
        m_overlayFn(cmd);
    }
    cmd->endRenderPass();
}

// M4.2 验证设施：同帧 A/B 对比（调试用）
void RenderSystem::selfTestAB() {
    static int frame = 0;
    if (++frame != 120) return;
    if (!m_rhiDevice) return;
    const auto fb = m_rhiDevice->framebufferSize();   // C1b：设备尺寸，不查窗口
    const int w = static_cast<int>(fb.width);
    const int h = static_cast<int>(fb.height);
    if (w <= 0 || h <= 0) return;

    static std::vector<unsigned char> fbA, fbB;
    auto grabFB = [&]() {
        fbA.swap(fbB);
        fbB.resize(static_cast<size_t>(w) * h * 4);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, fbB.data());
    };

    const bool original = m_settings.modelUseRHI;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_settings.modelUseRHI = true;
    renderScenePasses();
    grabFB();
    const std::vector<unsigned char> frameRHI = fbB;
    m_settings.modelUseRHI = false;
    renderScenePasses();
    grabFB();
    const std::vector<unsigned char> frameGL = fbB;

    // 差异统计
    int diffCount = 0;
    int minX = w, minY = h, maxX = -1, maxY = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            if (memcmp(&frameRHI[i], &frameGL[i], 4) != 0) {
                ++diffCount;
                if (x < minX) minX = x;
                if (x > maxX) maxX = x;
                if (y < minY) minY = y;
                if (y > maxY) maxY = y;
            }
        }
    }
    printf("[SELFTEST] model A/B diff pixels=%d bbox=(%d,%d)-(%d,%d) frame=%dx%d\n",
           diffCount, minX, minY, maxX, maxY, w, h);
    // 调试设施：导出 RHI 分支帧供目视检查（A2：跨平台临时目录，不写死 /tmp）
    {
        std::error_code ec;
        auto tmpDir = std::filesystem::temp_directory_path(ec);
        if (ec) tmpDir = ".";
        // A4: 保持 path 对象直接打开（临时目录可能含中文用户名，Windows ACP fopen 会失败）
        FILE* fp = engine::fs::openWrite(tmpDir / "frame_rhi.ppm");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; --y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                    fwrite(&frameRHI[i], 1, 3, fp);
                }
            }
            fclose(fp);
        }
        fp = engine::fs::openWrite(tmpDir / "frame_gl.ppm");
        if (fp) {
            fprintf(fp, "P6\n%d %d\n255\n", w, h);
            for (int y = h - 1; y >= 0; --y) {
                for (int x = 0; x < w; ++x) {
                    const size_t i = (static_cast<size_t>(y) * w + x) * 4;
                    fwrite(&frameGL[i], 1, 3, fp);
                }
            }
            fclose(fp);
        }
    }
    int printed = 0;
    for (int y = 0; y < h && printed < 6; ++y) {
        for (int x = 0; x < w && printed < 6; ++x) {
            const size_t i = (static_cast<size_t>(y) * w + x) * 4;
            if (memcmp(&frameRHI[i], &frameGL[i], 4) != 0) {
                printf("[SELFTEST]   sample (%d,%d) RHI=%d,%d,%d,%d GL=%d,%d,%d,%d\n",
                       x, y,
                       frameRHI[i], frameRHI[i+1], frameRHI[i+2], frameRHI[i+3],
                       frameGL[i], frameGL[i+1], frameGL[i+2], frameGL[i+3]);
                ++printed;
            }
        }
    }
    m_settings.modelUseRHI = original;
    std::fflush(stdout);
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
        // GL：sRGB RT 写入需 GL_FRAMEBUFFER_SRGB 开启才编码（VK 由格式驱动，无此状态）。
        // 保存/恢复开关，保证 Pass 对周边状态中性。
        GLboolean srgbWasOn = GL_FALSE;
        if (_backend == rhi::Backend::OpenGL) {
            srgbWasOn = glIsEnabled(GL_FRAMEBUFFER_SRGB);
            if (!srgbWasOn) glEnable(GL_FRAMEBUFFER_SRGB);
        }
        rhi::RenderTargetDesc rtd;
        rtd.width  = 128;
        rtd.height = 128;
        // VK dynamic rendering 契约：管线创建时烘焙附件格式（PipelineRenderingCreateInfo
        // = swapchain 格式），RT 颜色/深度格式必须与管线兼容 → 用交换链同款格式。
        // （跨格式 RT 需要"管线格式集"，属 D5 显示模式阶段的管线变体工作）
        rtd.colorFormat = rhi::Format::BGRA8_SRGB;
        rtd.depthFormat = rhi::Format::Depth32F;
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
        recordSmokeTriangle(cmd);
        cmd->endRenderPass();
        if (_backend == rhi::Backend::OpenGL && !srgbWasOn) {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }
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

void RenderSystem::requestPick(double x, double y) {
    if (!m_rhiDevice) return;
    // 窗口逻辑坐标 → framebuffer 像素坐标（Retina 下两者不同；比例来自
    // InitDesc 的 windowWidth/windowHeight，C1b 起引擎不查询窗口系统）
    if (m_windowW <= 0 || m_windowH <= 0) return;
    const double sx = static_cast<double>(m_frameW) / m_windowW;
    const double sy = static_cast<double>(m_frameH) / m_windowH;
    m_pickReq.pending  = true;
    m_pickReq.rendered = false;
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
        // GL：sRGB RT 写入需 GL_FRAMEBUFFER_SRGB（同 B4 harness，保存/恢复）
        GLboolean srgbWasOn = GL_FALSE;
        if (_backend == rhi::Backend::OpenGL) {
            srgbWasOn = glIsEnabled(GL_FRAMEBUFFER_SRGB);
            if (!srgbWasOn) glEnable(GL_FRAMEBUFFER_SRGB);
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
        if (_backend == rhi::Backend::OpenGL && !srgbWasOn) {
            glDisable(GL_FRAMEBUFFER_SRGB);
        }
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
        ENGINE_LOG_INFO("[PICK] no hit at (%u,%u)", rb.x, rb.y);
        return;
    }
    // BGRA 序：R=px[2]（ID 低字节） G=px[1]（ID 高字节）
    const uint16_t hitId = static_cast<uint16_t>(px[2] | (px[1] << 8));
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

void RenderSystem::drawSceneOverlays() {
    // RHI 冒烟测试三角形（开关在调试面板）
    if (m_settings.rhiSmokeTriangle && m_rhiDevice) {
        drawSmokeTriangle();
    }
    // RHI 贴图四边形（M4：纹理采样链路验证）
    if (m_settings.rhiTexQuad && m_rhiDevice) {
        drawTexturedQuad();
    }
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

// C1b：叠加内容并入主帧（renderFrame 已 beginFrame；GL 路径直接在默认帧缓冲
// 录制 pass，beginRenderPass 的状态快照保证对遗留 GL 代码状态中性）
void RenderSystem::drawTexturedQuad() {
    auto* cmd = m_rhiDevice->commandList();
    if (!cmd || !m_quadPipeline.valid() || !m_quadVB.valid() || !m_quadTex.valid()) return;

    rhi::RenderPassDesc rp;
    rp.clearColor = false;
    rp.clearDepth = false;
    cmd->beginRenderPass(rp);
    recordTexturedQuad(cmd);
    cmd->endRenderPass();
}

void RenderSystem::recordTexturedQuad(rhi::IRHICommandList* cmd) {
    if (!cmd || !m_quadPipeline.valid() || !m_quadVB.valid() || !m_quadIB.valid() || !m_quadTex.valid()) return;

    cmd->pushDebugLabel("RHITexQuad");
    cmd->bindPipeline(m_quadPipeline);
    cmd->setUniform("uMatrixMVP", rhi::UniformValue::fromMat4(kIdentityMat4));
    cmd->setUniform("uTex", rhi::UniformValue::fromInt(0));
    // VK：切到四边形专属 uniform 块后再绑纹理。
    // bindTexture 写入“当前块”的描述符集，沿用模型块会污染模型纹理槽。
    // GL 后端按名直传，不需要此步骤。
    auto* device = RenderSystem::getInstance().rhiDevice();
    if (device && device->backend() == rhi::Backend::Vulkan) {
        if (!m_quadUB.valid()) return;   // 无专属块时宁可不画，避免污染模型描述符集
        static const unsigned char zeroBlock[256] = {};
        cmd->setDrawUniforms(m_quadUB, zeroBlock, sizeof(zeroBlock));
    }
    cmd->bindTexture(m_quadTex, 0);
    cmd->bindVertexBuffer(m_quadVB);
    cmd->bindIndexBuffer(m_quadIB);
    cmd->drawIndexed(6);
    cmd->popDebugLabel();
}

void RenderSystem::renderScenePasses() {
    // Vulkan 模式：旧 GL Pass 整体跳过（无 GL 上下文，任何 glad 调用都会跳空地址）。
    // RHI 绘制的内容（冒烟三角形/已迁移对象）正常渲染；Pass 迁移在 M4 逐个进行。
    if (_backend == rhi::Backend::Vulkan) {
        return;
    }
    for (auto pass : m_vec_renderpass)
    {
        if (auto shadow = std::dynamic_pointer_cast<CShadowPass>(pass)) {
            (void)shadow;
            if (!m_settings.drawShadowPass) continue;
        }
        else if (auto sky = std::dynamic_pointer_cast<CSkyPass>(pass)) {
            (void)sky;
            if (!m_settings.drawSkyPass) continue;
        }
        else if (auto pick = std::dynamic_pointer_cast<CPickPass>(pass)) {
            (void)pick;
            if (!m_settings.drawPickPass) continue;
        }
        pass->draw(mCurScene.get());
    }
}


void RenderSystem::drawSmokeTriangle() {
    auto* cmd = m_rhiDevice->commandList();
    if (!cmd || !m_smokePipeline.valid() || !m_smokeVB.valid()) return;

    rhi::RenderPassDesc rp;
    rp.clearColor = false;      // 叠加在主场景上，不清屏
    rp.clearDepth = false;
    cmd->beginRenderPass(rp);
    recordSmokeTriangle(cmd);
    cmd->endRenderPass();
}

void RenderSystem::recordSmokeTriangle(rhi::IRHICommandList* cmd) {
    if (!cmd || !m_smokePipeline.valid() || !m_smokeVB.valid()) return;

    cmd->pushDebugLabel("RHISmokeTriangle");
    cmd->bindPipeline(m_smokePipeline);
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
        // D2: RHI 拾取（有 RHI 设备时走 ID 图回读；GL 遗留拾取并行保留）
        if (_backend == rhi::Backend::Vulkan || m_settings.modelUseRHI) {
            requestPick(x, y);
        }
    }
}

void RenderSystem::onMouseRightDown(double x, double y) {
}

void RenderSystem::onMouseRightUp(double x, double y) {
}

void RenderSystem::onMouseMiddleDown(double x, double y) {
}

void RenderSystem::onMouseMove(double x, double y) {
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
    mRenderContent->screenWidth = x;
    mRenderContent->screenHeight = y;
    if (m_rhiDevice) {
        m_rhiDevice->onResize(x, y);   // Vulkan: swapchain 重建
    }
    
    auto camera = mCurScene->GetActiveCamera();
    camera->setViewport(x, y);
}
