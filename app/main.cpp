#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include "3rdparty/imgui/imgui.h"
#include "backends/imgui_impl_glfw.h"
// A2: Vulkan 相关代码按构建能力裁剪（GL-only 构建不依赖 Vulkan SDK）
#ifndef ENGINE_HAS_VULKAN
#define ENGINE_HAS_VULKAN 0
#endif
#if ENGINE_HAS_VULKAN
#include "backends/imgui_impl_vulkan.h"
#endif
#include "backends/imgui_impl_opengl3.h"
#include <iostream>
#include <cstdlib>
#if ENGINE_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif
#include <cstring>
#include <filesystem>
#include "camera/camera.h"
#include "RenderSystem.h"
#include "scene.h"
#include "object/Line.h"
#include "object/Sky.h"
#include "object/Text.h"
#include "object/ImageRectangle.h"
#include "model/model.h"
#include "Light.h"
#include "taskQueue.h"
#include "FontManager.h"
#include "app/DebugPanel.h"
#include "rhi/IRHICommandList.h"
#include "rhi/IRHIDevice.h"   // UI 层：ImGui 面板只在 app 层使用，不进引擎库
#include "color/ColorSpace.h"
#include "fs/FilePathUtil.h"
#include "debug/CrashBacktrace.h"

void framebuffer_size_callback(GLFWwindow* window, int width, int height);
void mouse_callback(GLFWwindow* window, double xpos, double ypos);
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset);
void processInput(GLFWwindow* window);
void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods);
void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods);
void testRenderSystem(const std::string &rootPath, bool useVulkan);
// settings
const unsigned int SCR_WIDTH = 1600;
const unsigned int SCR_HEIGHT = 900;

// camera
glm::vec3 cameraPos = glm::vec3(0.0f, 0.0f, 3.0f);
glm::vec3 cameraFront = glm::vec3(0.0f, 0.0f, -1.0f);
glm::vec3 cameraUp = glm::vec3(0.0f, 1.0f, 0.0f);

bool firstMouse = true;
float yaw = -90.0f;    // yaw is initialized to -90.0 degrees since a yaw of 0.0 results in a direction vector pointing to the right so we initially rotate a bit to the left.
float pitch = 0.0f;
float lastX = 800.0f / 2.0;
float lastY = 600.0 / 2.0;
float fov = 45.0f;

#if ENGINE_HAS_VULKAN
static VkDescriptorPool g_imguiDescriptorPool = VK_NULL_HANDLE;
#endif

int main()
{
    // RHI_DEBUG_LOG=1：stdout 无缓冲（崩溃/abort 时 MoltenVK 内部日志不丢）+
    // SIGABRT/SIGSEGV 打印 backtrace（MVK 内部静默 abort 的定位手段）
    if (std::getenv("RHI_DEBUG_LOG")) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        engine::debug::installCrashBacktrace();
    }

    // glfw: initialize and configure
    // ------------------------------
    glfwInit();

    // 后端选择：RHI_BACKEND 环境变量（opengl=默认 / vulkan）
    bool useVulkan = false;
    if (const char* b = std::getenv("RHI_BACKEND")) {
        useVulkan = (std::strcmp(b, "vulkan") == 0 || std::strcmp(b, "VULKAN") == 0);
    }
#if !ENGINE_HAS_VULKAN
    if (useVulkan) {
        std::printf("[UI] RHI_BACKEND=vulkan but this build has no Vulkan support; "
                    "falling back to OpenGL\n");
        useVulkan = false;
    }
#endif

    if (useVulkan) {
        // Vulkan 模式：窗口不创建 GL 上下文，surface 由 RHI 层创建
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    } else {
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        // B3c：线性工作流要求默认帧缓冲可做 sRGB 编码
        glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_TRUE);
    }

#ifdef __APPLE__
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif

    // glfw window creation
    // --------------------
    GLFWwindow* window = glfwCreateWindow(SCR_WIDTH, SCR_HEIGHT, "LearnOpenGL", NULL, NULL);
    if (window == NULL)
    {
        std::cout << "Failed to create GLFW window" << std::endl;
        glfwTerminate();
        return -1;
    }
    glfwMakeContextCurrent(window);
    glfwSetFramebufferSizeCallback(window, framebuffer_size_callback);
    glfwSetCursorPosCallback(window, mouse_callback);
    glfwSetScrollCallback(window, scroll_callback);
    glfwSetKeyCallback(window, key_callback);
    glfwSetMouseButtonCallback(window, mouseButtonCallback);
    //glfwSetInputMode(window, GLFW_CURSOR_HIDDEN, GLFW_CURSOR_HIDDEN);
    //glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
    //InputProcessMng::getInstance().setFrameBufferSize(width, height);

    // glad: load all OpenGL function pointers
    // ---------------------------------------
    if (!useVulkan && !gladLoadGLLoader((GLADloadproc)glfwGetProcAddress))
    {
        std::cout << "Failed to initialize GLAD" << std::endl;
        return -1;
    }
    if (!useVulkan) {
        // B3c：GLFW 不报告 GLFW_SRGB_CAPABLE（macOS 所有像素格式都支持 sRGB，
        // 无该查询项），直接向默认帧缓冲询问颜色编码。
        GLint encoding = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glGetFramebufferAttachmentParameteriv(
            GL_FRAMEBUFFER, GL_BACK, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
        std::printf("[GL] default framebuffer color encoding = %s\n",
                    encoding == GL_SRGB ? "sRGB" : "linear");
        // 诊断：GL_FRAMEBUFFER_SRGB 是否真的生效（0.5 线性 → sRGB ≈ 188）
        glEnable(GL_FRAMEBUFFER_SRGB);
        glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        unsigned char probe[4] = {0, 0, 0, 0};
        glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, probe);
        glDisable(GL_FRAMEBUFFER_SRGB);
        std::printf("[GL] sRGB encode probe = %d (128=linear, 188=sRGB)\n", probe[0]);
    }
    
    // ��ʼ�� ImGui
    // 两模式均有 ImGui UI：GL 用 OpenGL3 backend；Vulkan backend 需 RHI 设备，
    // 延后到 RenderSystem::init 之后初始化（见下方 useVulkan 分支）
    if (true) {
        IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();

    // ��ʼ�� ImGui ƽ̨/��Ⱦ����
    if (!useVulkan) {
        ImGui_ImplGlfw_InitForOpenGL(window, true);
        ImGui_ImplOpenGL3_Init("#version 150");
    }

    // 中文字体支持：默认 ProggyClean 无 CJK 字形。注意 macOS 的 PingFang.ttc
    // 是 CFF 轮廓（stb_truetype 不支持），必须用 TrueType 轮廓的中文字体
    {
        const char* fontCandidates[] = {
#if defined(__APPLE__)
            "/System/Library/Fonts/STHeiti Medium.ttc",      // 黑体（TrueType 轮廓）
            "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
            "/Library/Fonts/Arial Unicode.ttf",
#elif defined(_WIN32)
            "C:/Windows/Fonts/msyh.ttc",                     // 微软雅黑
            "C:/Windows/Fonts/msyh.ttf",
            "C:/Windows/Fonts/simhei.ttf",                   // 黑体
            "C:/Windows/Fonts/simsun.ttc",                   // 宋体
#elif defined(__linux__)
            "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
            "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
#endif
        };
        bool fontLoaded = false;
        for (const char* f : fontCandidates) {
            if (std::filesystem::exists(f)) {
                ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(
                    f, 17.0f, nullptr,
                    ImGui::GetIO().Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (font) {
                    std::printf("[UI] CJK font loaded: %s\n", f);
                    fontLoaded = true;
                    break;
                }
                std::printf("[UI] font bake failed: %s\n", f);
            }
        }
        if (!fontLoaded) std::printf("[UI] WARNING: no CJK font loaded, UI will show garbage\n");
    }
    }

    // ��ȡ��Ŀ¼
    // A4: 路径字符串统一按 UTF-8 传递（Windows 上 path::string() 是 ACP，
    // 中文安装路径在字符串层就会坏）
    std::filesystem::path current_path = std::filesystem::current_path();
    std::string proPath = engine::fs::utf8FromPath(current_path);
    char sep = std::filesystem::path::preferred_separator;
    std::string buildFolder = std::string(1, sep) + "build";
    size_t pos =  proPath.find(buildFolder);
    proPath = proPath.substr(0, pos);
    // ��ʼ����Ⱦ����
    int width, height;
    glfwGetFramebufferSize(window, &width, &height);
    RenderSystem::getInstance().setWindow(window);   // RHI: must be called before init
    RenderSystem::getInstance().init(proPath);
    // 资源加载是 app 层职责（引擎不假设项目资源布局）
    {
        auto& renderSys = RenderSystem::getInstance();
        renderSys.preloadTextures(engine::fs::utf8FromPath(engine::fs::pathFromUtf8(proPath) / "res"));
        // 字体字形当前是 GL 纹理实现，仅 GL 模式加载（Vulkan 文字渲染 M4 迁移）
        if (!useVulkan) {
            renderSys.GetFontMng()->LoadFont(
                engine::fs::utf8FromPath(engine::fs::pathFromUtf8(proPath) / "res" / "font" / "arial.ttf"), 48);
        }
    }
    RenderSystem::getInstance().onWindowSizeChanged(width, height);

    // Vulkan 模式：ImGui Vulkan backend（需要 RHI 设备，故在 init 之后初始化）
#if ENGINE_HAS_VULKAN
    if (useVulkan) {
        auto* dev = RenderSystem::getInstance().rhiDevice();
        rhi::IRHIDevice::NativeHandles h;
        if (!dev || !dev->nativeHandles(h)) {
            std::cout << "VK: no native handles for ImGui" << std::endl;
            return -1;
        }
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000},
            {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000},
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000},
        };
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        pi.maxSets = 1000;
        pi.poolSizeCount = 8;
        pi.pPoolSizes = sizes;
        if (vkCreateDescriptorPool((VkDevice)h.device, &pi, nullptr, &g_imguiDescriptorPool) != VK_SUCCESS) {
            std::cout << "Failed to create ImGui descriptor pool" << std::endl;
            return -1;
        }
        ImGui_ImplGlfw_InitForVulkan(window, true);
        // B1：引擎走 dynamic rendering（无 renderpass），ImGui 用同模式接入。
        // pColorAttachmentFormats 指向的变量必须比 Init 存活 → static
        static VkFormat s_imguiColorFormat = VK_FORMAT_UNDEFINED;
        s_imguiColorFormat = (VkFormat)h.colorFormat;
        ImGui_ImplVulkan_InitInfo init{};
        init.Instance = (VkInstance)h.instance;
        init.PhysicalDevice = (VkPhysicalDevice)h.physicalDevice;
        init.Device = (VkDevice)h.device;
        init.QueueFamily = h.queueFamily;
        init.Queue = (VkQueue)h.queue;
        init.DescriptorPool = g_imguiDescriptorPool;
        init.MinImageCount = h.minImageCount;
        init.ImageCount = h.imageCount;
        init.UseDynamicRendering = true;
        init.PipelineRenderingCreateInfo = {};
        init.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        init.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_imguiColorFormat;
        init.PipelineRenderingCreateInfo.depthAttachmentFormat = (VkFormat)h.depthFormat;
        init.CheckVkResultFn = [](VkResult err) {
            if (err != VK_SUCCESS) std::printf("[IMGUI-VK] CheckVkResult %d\n", (int)err);
        };
        ImGui_ImplVulkan_Init(&init);

        // B3c：VK 交换链为 sRGB，写回时硬件做 linear→sRGB 编码。ImGui 样式色
        // 是 sRGB 作者值，若直接写会被二次编码（UI 发白）→ 预转线性。
        {
            ImVec4* colors = ImGui::GetStyle().Colors;
            for (int i = 0; i < ImGuiCol_COUNT; ++i) {
                colors[i].x = color::srgbToLinear(colors[i].x);
                colors[i].y = color::srgbToLinear(colors[i].y);
                colors[i].z = color::srgbToLinear(colors[i].z);
            }
        }

        // UI 在 RHI render pass 内渲染（场景之后）
        RenderSystem::getInstance().setOverlayRenderer([](rhi::IRHICommandList* cmd) {
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(),
                                            reinterpret_cast<VkCommandBuffer>(cmd->nativeCommandBuffer()));
        });
    }
#endif

    // 使用rendersysytem的各种接口
    testRenderSystem(proPath, useVulkan);

    // 冒烟/回归：RHI_EXIT_AFTER=<帧数> 后正常关闭窗口（走完整析构，验证缓存落盘等）
    long exitAfterFrames = -1;
    if (const char* e = std::getenv("RHI_EXIT_AFTER")) exitAfterFrames = std::atol(e);
    // B1 验证：RHI_RESIZE_AT=<帧数> 在指定帧改变窗口尺寸（验证 swapchain 重建
    // 不再重建 PSO；dynamic rendering 后管线与 framebuffer 解耦）
    long resizeAtFrame = -1;
    if (const char* e = std::getenv("RHI_RESIZE_AT")) resizeAtFrame = std::atol(e);
    long frameIndex = 0;

    while (!glfwWindowShouldClose(window))
    {
        // input
        processInput(window);
        if (exitAfterFrames >= 0 && ++frameIndex >= exitAfterFrames) {
            glfwSetWindowShouldClose(window, true);
        }

        if (resizeAtFrame > 0 && frameIndex == resizeAtFrame) {
            std::printf("[UI] resize test: window -> 1100x700 at frame %ld\n", frameIndex);
            std::fflush(stdout);
            glfwSetWindowSize(window, 1100, 700);
        }

        RenderSystem::getInstance().update();

#if ENGINE_HAS_VULKAN
        // Vulkan mode: build ImGui frame before draw (RenderDrawData runs inside drawRhiScene's pass)
        if (useVulkan) {
            ImGui_ImplGlfw_NewFrame();      // 先设置 DisplaySize（依赖 GLFW 窗口）
            ImGui_ImplVulkan_NewFrame();
            ImGui::NewFrame();
            {
                auto& renderSys = RenderSystem::getInstance();
                DebugPanel::draw(renderSys.settings(), renderSys.rhiDevice());
            }
            ImGui::Render();
        }
#endif

        RenderSystem::getInstance().draw();

        if (!useVulkan) {
            // B3c：ImGui（GL backend）保持既有 gamma 空间外观；场景已在
            // RenderSystem::draw() 内开启 GL_FRAMEBUFFER_SRGB。
            glDisable(GL_FRAMEBUFFER_SRGB);
            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            {
                auto& renderSys = RenderSystem::getInstance();
                DebugPanel::draw(renderSys.settings(), renderSys.rhiDevice());
            }
            ImGui::Render();
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        }

        // glfw: swap buffers and poll IO events (keys pressed/released, mouse moved etc.)
        // -------------------------------------------------------------------------------
        // Vulkan 模式：present 由 RHI endFrame 完成，且 NO_API 窗口无 GL 上下文可 swap
        if (!useVulkan) {
            glfwSwapBuffers(window);
        }
        glfwPollEvents();
    }

#if ENGINE_HAS_VULKAN
    if (useVulkan) {
        // ImGui 的资源（buffer/pipeline/描述符集）被最后一帧的命令缓冲引用，
        // 销毁前必须等 GPU 完成；否则验证层报 "currently in use"
        if (auto* dev = RenderSystem::getInstance().rhiDevice()) dev->waitIdle();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        if (g_imguiDescriptorPool != VK_NULL_HANDLE) {
            auto* dev = RenderSystem::getInstance().rhiDevice();
            rhi::IRHIDevice::NativeHandles h;
            if (dev && dev->nativeHandles(h)) {
                vkDestroyDescriptorPool((VkDevice)h.device, g_imguiDescriptorPool, nullptr);
            }
        }
    } else
#endif
    {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
    }

    // RHI 设备/GL 资源必须在窗口销毁前显式释放（单例静态析构太晚，会崩溃；
    // Vulkan 后端还依赖此处导出 pipeline cache）
    RenderSystem::getInstance().shutdown();

    // glfw: terminate, clearing all previously allocated GLFW resources.
    // ------------------------------------------------------------------
    glfwTerminate();
    return 0;
}

// process all input: query GLFW whether relevant keys are pressed/released this frame and react accordingly
// ---------------------------------------------------------------------------------------------------------
void processInput(GLFWwindow *window)
{
    if(glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        glfwSetWindowShouldClose(window, true);
}

// glfw: whenever the window size changed (by OS or user resize) this callback function executes
// ---------------------------------------------------------------------------------------------
void framebuffer_size_callback(GLFWwindow* window, int width, int height)
{
    // make sure the viewport matches the new window dimensions; note that width and 
    // height will be significantly larger than specified on retina displays.
    RenderSystem::getInstance().onWindowSizeChanged(width, height);
    
}

void mouse_callback(GLFWwindow* window, double xposIn, double yposIn)
{
    RenderSystem::getInstance().onMouseMove(xposIn, yposIn);
}

// glfw: whenever the mouse scroll wheel scrolls, this callback is called
// ----------------------------------------------------------------------
void scroll_callback(GLFWwindow* window, double xoffset, double yoffset)
{
    RenderSystem::getInstance().onMouseMiddleScroll(xoffset, yoffset);
}

void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
    if (action == GLFW_PRESS)
    {
        //std::cout << "Key pressed: " << key << std::endl;
    }
    else if (action == GLFW_RELEASE)
    {
        if (GLFW_KEY_C == key)
        {
            //RenderSystem::getInstance().onKeyRelease_C();
        }
    }
}

// ������ص�����
void mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);

    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_RELEASE)
        {
            RenderSystem::getInstance().onMouseLeftUp(xpos, ypos);
        }
        else if (action == GLFW_PRESS)
        {
            RenderSystem::getInstance().onMouseLeftDown(xpos, ypos);
        }
    }

    else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_RELEASE)
        {
            RenderSystem::getInstance().onMouseRightUp(xpos, ypos);
        }
        else if (action == GLFW_PRESS)
        {
            RenderSystem::getInstance().onMouseRightDown(xpos, ypos);
        }
    }
    else if (button == GLFW_MOUSE_BUTTON_MIDDLE) {
        if (action == GLFW_PRESS)
        {
            RenderSystem::getInstance().onMouseMiddleDown(xpos, ypos);
        }
    }
}

void testRenderSystem(const std::string &rootPath, bool useVulkan) {
    auto pScene = RenderSystem::getInstance().GetCurScene();
    if (!pScene)
    {
        return;
    }
    
    // 绘制坐标轴
    std::vector<unsigned int> indices = {0, 1};
    
    float len = 15;
    glm::vec3 pZero = {0, 0, 0};
    glm::vec3 pX = {len, 0, 0};
    glm::vec3 pY = {0, len, 0};
    glm::vec3 pZ = {0, 0, len};
    // 3个轴
    std::shared_ptr<Line> line_x = std::make_shared<Line>();
    std::shared_ptr<Line> line_y = std::make_shared<Line>();
    std::shared_ptr<Line> line_z = std::make_shared<Line>();
    line_x->setData({pZero, pX}, indices);
    line_y->setData({pZero, pY}, indices);
    line_z->setData({pZero, pZ}, indices);
    
    // 颜色
    line_x->setColor({1, 0, 0});
    line_y->setColor({0, 1, 0});
    line_z->setColor({0, 0, 1});
    
    auto start = std::chrono::high_resolution_clock::now();
    // 地面（GL 专属：calculate 混合 CPU/GPU 操作，VK 模式跳过；未加入场景）
    std::shared_ptr<ImageRectangle> objGround = nullptr;
    if (!useVulkan) {
        objGround = std::make_shared<ImageRectangle>();
    }
    [[maybe_unused]] std::shared_ptr<ImageRectangle> objGroundUnused = objGround;
    float ground_width = 10;
    auto texPath = engine::fs::pathFromUtf8(rootPath) / "res" / "textures";
    if (objGround) {
        objGround->setImagePath(engine::fs::utf8FromPath(texPath / "bricks2.jpg"),
            engine::fs::utf8FromPath(texPath / "bricks2_normal.jpg"),
            engine::fs::utf8FromPath(texPath / "bricks2_disp.jpg"));

        objGround->setSetp(5, 5);
        glm::vec3 p1(-ground_width, 0,  ground_width);
        glm::vec3 p2(ground_width, 0,  ground_width);
        glm::vec3 p3(ground_width, 0, -ground_width);
        glm::vec3 p4(-ground_width, 0, -ground_width);
        objGround->setPoints(p1, p2, p3, p4);
        objGround->calculate();
    }
    
    // 鸭子
    std::shared_ptr<Model> objDuck = std::make_shared<Model>();
    objDuck->LoadFile(engine::fs::utf8FromPath(engine::fs::pathFromUtf8(rootPath) / "res" / "model" / "duck.dae"));
    TaskQueue::instance().pushTask([start](){
    }); 
    
    objDuck->setCount(4);
    objDuck->setPosition(0, {0, 0, 2});
    objDuck->setScale(0, 0.01);
    objDuck->setPosition(1, {-2, 0, 2});
    objDuck->setScale(1, 0.01);
    objDuck->setPosition(2, {2, 0, 2});
    objDuck->setScale(2, 0.01);
    objDuck->setPosition(3, {0, 0, -2});
    objDuck->setScale(3, 0.01);
    objDuck->setRotateY(3, 90);
    
    // 光源模型
    std::shared_ptr<Model> objLight = std::make_shared<Model>();
    objLight->LoadFile(engine::fs::utf8FromPath(engine::fs::pathFromUtf8(rootPath) / "res" / "model" / "duck.dae"));
    objLight->setCount(1);
    objLight->setLightOpen(false);
    auto lightPos = Light::GlobalLight().position;
    objLight->setPosition(0, {lightPos.x, lightPos.y, lightPos.z});
    objLight->setScale(0, 0.5);
    
    //
    auto skyboxPath = texPath / "skybox";
    std::shared_ptr<Sky> objSky = std::make_shared<Sky>();
    objSky->setCubeImage({
        engine::fs::utf8FromPath(skyboxPath / "right.jpg"),
        engine::fs::utf8FromPath(skyboxPath / "left.jpg"),
        engine::fs::utf8FromPath(skyboxPath / "top.jpg"),
        engine::fs::utf8FromPath(skyboxPath / "bottom.jpg"),
        engine::fs::utf8FromPath(skyboxPath / "front.jpg"),
        engine::fs::utf8FromPath(skyboxPath / "back.jpg")
    });
    
    // obj
    pScene->AddObj(objDuck);
    pScene->AddObj(objLight);
    pScene->AddObj(line_x);
    pScene->AddObj(line_y);
    pScene->AddObj(line_z);

    // 天空盒
    pScene->SetSkyBox(objSky);

    // 文字（GL 专属：字形纹理为 GL 实现，VK 文字渲染 M4.3 迁移）
    auto& renderSys = RenderSystem::getInstance();
    if (!useVulkan) {
        auto fontMng = renderSys.GetFontMng();
        std::shared_ptr<CText> objText = std::make_shared<CText>(std::weak_ptr<FontManager>(fontMng));
        objText->SetText("test");
        pScene->AddObj(objText);
    }
}
