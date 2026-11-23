#include "log/LogMacros.h"
#include "RenderSystem.h"
#include "render/scene.h"
#include "object/Sky.h"
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
#include <GLFW/glfw3.h>
#include <cstdlib>
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

void RenderSystem::init(const std::string& rootPath) {
    // 后端选择：RHI_BACKEND 环境变量（opengl=默认 / vulkan）
    if (const char* b = std::getenv("RHI_BACKEND")) {
        if (std::strcmp(b, "vulkan") == 0 || std::strcmp(b, "VULKAN") == 0) {
            _backend = rhi::Backend::Vulkan;
            ENGINE_LOG_INFO("[RHI] using Vulkan backend (MoltenVK)");
        }
    }

    // ������ɫ��
    auto shaderPath = std::filesystem::path(rootPath) / "res" / "shaders";
    // 注意：GL 着色器缓存仅在 OpenGL 后端初始化（Vulkan 模式无 GL 上下文，
    // glad 函数指针为空，任何 glCreate* 调用都会跳空地址）
    if (_backend == rhi::Backend::OpenGL) {
        ShaderCache::GetInstance().init(shaderPath.string() + std::string(1, std::filesystem::path::preferred_separator));
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

    // ---- 创建 RHI 设备（RHI_BACKEND 环境变量选择：opengl[默认] / vulkan）----
    if (_window) {
        std::filesystem::path shaderRoot = shaderPath;
        if (_backend == rhi::Backend::Vulkan) {
            shaderRoot = shaderPath.parent_path() / "shaders_vk";
        }
        m_rhiDevice = rhi::RHIFactory::createDevice(
            _backend, _window,
            shaderRoot.string() + std::string(1, std::filesystem::path::preferred_separator));
        if (m_rhiDevice) {
            createSmokeTriangle();
            createTexturedQuad();
            // 无 UI 环境（如 Vulkan 模式早期验证）可强制显示验证内容
            if (std::getenv("RHI_SMOKE")) {
                m_settings.rhiSmokeTriangle = true;
            }
            if (std::getenv("RHI_TEXQUAD")) {
                m_settings.rhiTexQuad = true;
            }
        }
    }
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

    rhi::BufferDesc vbDesc;
    vbDesc.size = sizeof(kSmokeVertices);
    vbDesc.usage = rhi::BUF_VERTEX;
    vbDesc.initialData = kSmokeVertices;
    m_smokeVB = m_rhiDevice->createBuffer(vbDesc);

    // 索引 0,1,2：走 drawIndexed（M3 Vulkan 验证用）
    static const unsigned int kSmokeIndices[3] = {0, 1, 2};
    rhi::BufferDesc ibDesc;
    ibDesc.size = sizeof(kSmokeIndices);
    ibDesc.usage = rhi::BUF_INDEX;
    ibDesc.initialData = kSmokeIndices;
    m_smokeIB = m_rhiDevice->createBuffer(ibDesc);
}

void RenderSystem::update() {

}

void RenderSystem::BeginDraw() {

}

void RenderSystem::drawRhiScene() {
    if (!m_rhiDevice || !mCurScene) return;
    auto* cmd = m_rhiDevice->commandList();
    if (!cmd) return;

    m_rhiDevice->beginFrame();
    rhi::RenderPassDesc rp;
    rp.clearColor = true;
    rp.clearDepth = true;
    rp.clearColorValue[0] = 0.2f; rp.clearColorValue[1] = 0.3f; rp.clearColorValue[2] = 0.3f; rp.clearColorValue[3] = 1.0f;
    cmd->beginRenderPass(rp);
    cmd->pushDebugLabel("RhiScene");
    static bool emptyScene = (std::getenv("RHI_EMPTY") != nullptr);   // 调试开关：仅清屏+UI（VK 面板排查用）
    static bool once = false;
    if (!once) { once = true; ENGINE_LOG_ERROR("[SCENE] emptyScene=%d objs=%zu", emptyScene ? 1 : 0, mCurScene->GetObjs().size()); }
    if (!emptyScene) {
        if (auto cam = mCurScene->GetActiveCamera()) {
            auto vp = cam->GetVPMatrix();
            for (auto& obj : mCurScene->GetObjs()) {
                obj->draw(vp);
            }
            // 天空盒（RHI 路径，M4.3）
            if (auto sky = mCurScene->GetSkyBox()) {
                sky->drawRHI(cmd, cam->GetViewMatrix(), cam->GetProjectMatrix());
            }
        }
    }
    // UI 层叠加（ImGui Vulkan 等）在 render pass 内渲染
    if (m_overlayFn) {
        m_overlayFn(cmd);
    }
    cmd->endRenderPass();
    m_rhiDevice->endFrame();
}

void RenderSystem::draw() {
    // Vulkan 后端：GL Pass 未创建（VK 模式），走 RHI 场景绘制
    if (_backend == rhi::Backend::Vulkan) {
        drawRhiScene();
        return;
    }
    // 帧级 Pass 循环（调试面板可按开关跳过）
    renderScenePasses();
    drawSceneOverlays();
    selfTestAB();   // M4.2 验证设施：同帧 A/B 逐像素对比（调试用）
}

// M4.2 验证设施：同帧 A/B 对比（调试用）
void RenderSystem::selfTestAB() {
    static int frame = 0;
    if (++frame != 120) return;
    if (!_window) return;
    int w = 0, h = 0;
    glfwGetFramebufferSize(_window, &w, &h);
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
    // 调试设施：导出 RHI 分支帧供目视检查
    {
        FILE* fp = fopen("/tmp/frame_rhi.ppm", "wb");
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
        fp = fopen("/tmp/frame_gl.ppm", "wb");
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
    texDesc.initialData = purple;
    m_quadTex = m_rhiDevice->createTexture(texDesc);
}

void RenderSystem::drawTexturedQuad() {
    auto* cmd = m_rhiDevice->commandList();
    if (!cmd || !m_quadPipeline.valid() || !m_quadVB.valid() || !m_quadTex.valid()) return;

    m_rhiDevice->beginFrame();

    rhi::RenderPassDesc rp;
    rp.clearColor = false;
    rp.clearDepth = false;
    cmd->beginRenderPass(rp);
    cmd->pushDebugLabel("RHITexQuad");
    cmd->bindPipeline(m_quadPipeline);
    cmd->setUniform("uMatrixMVP", rhi::UniformValue::fromMat4(kIdentityMat4));
    cmd->setUniform("uTex", rhi::UniformValue::fromInt(0));
    cmd->bindTexture(m_quadTex, 0);
    cmd->bindVertexBuffer(m_quadVB);
    cmd->bindIndexBuffer(m_quadIB);
    cmd->drawIndexed(6);
    cmd->popDebugLabel();
    cmd->endRenderPass();

    m_rhiDevice->endFrame();
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

    m_rhiDevice->beginFrame();

    rhi::RenderPassDesc rp;
    rp.clearColor = false;      // 叠加在主场景上，不清屏
    rp.clearDepth = false;
    cmd->beginRenderPass(rp);
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
    cmd->endRenderPass();

    m_rhiDevice->endFrame();
}

void RenderSystem::preloadTextures(const std::string& dirPath) {
    loadTexture(std::filesystem::path(dirPath));
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
