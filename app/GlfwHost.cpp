//
//  GlfwHost.cpp
//
#include "GlfwHost.h"

#include <glad/glad.h>
#include <iostream>
#include <cstring>
#include <filesystem>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#if ENGINE_HAS_VULKAN
#include "backends/imgui_impl_vulkan.h"
#endif

#include "RenderSystem.h"
#include "rhi/RHIFactory.h"
#include "rhi/IRHIDevice.h"
#include "rhi/IRHICommandList.h"
#include "color/ColorSpace.h"

// GL 交换回调（静态函数指针形态，匹配 NativeWindow::swapBuffers 签名）
void GlfwHost::swapAdapter(void* user) {
    glfwSwapBuffers(static_cast<GLFWwindow*>(user));
}

GlfwHost::~GlfwHost() {
    if (_window) glfwDestroyWindow(_window);
}

bool GlfwHost::createWindow(int width, int height, bool useVulkan, std::string* outError) {
    if (!glfwInit()) {
        if (outError) *outError = "glfwInit failed";
        return false;
    }

    if (useVulkan) {
        // Vulkan 模式：窗口不创建 GL 上下文，surface 由宿主回调创建
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

    _window = glfwCreateWindow(width, height, "LearnOpenGL", nullptr, nullptr);
    if (!_window) {
        if (outError) *outError = "failed to create GLFW window";
        glfwTerminate();
        return false;
    }
    glfwMakeContextCurrent(_window);
    glfwSetFramebufferSizeCallback(_window, framebufferSizeCallback);
    glfwSetCursorPosCallback(_window, mouseCallback);
    glfwSetScrollCallback(_window, scrollCallback);
    glfwSetKeyCallback(_window, keyCallback);
    glfwSetMouseButtonCallback(_window, mouseButtonCallback);

    if (!useVulkan) {
        if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
            if (outError) *outError = "failed to initialize GLAD";
            return false;
        }
        // B3c：GLFW 不报告 GLFW_SRGB_CAPABLE（macOS 所有像素格式都支持 sRGB，
        // 无该查询项），直接向默认帧缓冲询问颜色编码。
        GLint encoding = 0;
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glGetFramebufferAttachmentParameteriv(
            GL_FRAMEBUFFER, GL_BACK_LEFT, GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING, &encoding);
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
    return true;
}

bool GlfwHost::buildDeviceDesc(bool useVulkan, std::string* outError) {
    int width = 0, height = 0, winW = 0, winH = 0;
    glfwGetFramebufferSize(_window, &width, &height);
    glfwGetWindowSize(_window, &winW, &winH);

    // ---- C1b/C5：DeviceDesc 组装（宿主职责）----
    // 窗口类型按平台（VK 建面在下方回调提供；GL 只用交换回调）
    rhi::DeviceDesc& desc = _deviceDesc;
    desc.backend = useVulkan ? rhi::Backend::Vulkan : rhi::Backend::OpenGL;
    desc.appName = "my_app";
    desc.width   = static_cast<uint32_t>(width);
    desc.height  = static_cast<uint32_t>(height);
#if defined(_WIN32)
    desc.window.type = rhi::NativeWindowType::Win32;
#elif defined(__APPLE__)
    desc.window.type = rhi::NativeWindowType::Cocoa;
#else
    desc.window.type = rhi::NativeWindowType::Xlib;
#endif
    desc.window.handle      = (void*)_window;
    desc.window.swapBuffers = swapAdapter;   // GL present
    desc.window.swapUser    = _window;
#if ENGINE_HAS_VULKAN
    // macOS/GLFW 的 surface 建面必须走宿主回调（引擎不碰 GLFW/ObjC++）
    if (useVulkan) {
        desc.createSurface = [win = _window](uint64_t instance) -> uint64_t {
            VkSurfaceKHR s = VK_NULL_HANDLE;
            if (glfwCreateWindowSurface(reinterpret_cast<VkInstance>(instance),
                                        win, nullptr, &s) != VK_SUCCESS) {
                return 0;
            }
            return reinterpret_cast<uint64_t>(s);
        };
    }
#endif
    // 后端变体着色器根目录（运行目录推导项目根；正式宿主应显式配置）
    {
        const std::filesystem::path cwd = std::filesystem::current_path();
        std::string proPath;
#if defined(__APPLE__) || defined(__linux__)
        proPath = cwd.string();
#else
        proPath = cwd.string();
#endif
        const std::string marker = "/build";
        const size_t pos = proPath.find(marker);
        if (pos != std::string::npos) proPath = proPath.substr(0, pos);
        const char sep = std::filesystem::path::preferred_separator;
        desc.shaderRootPath = proPath + sep + "res" + sep
                            + "shaders" + (useVulkan ? "_vk" : "") + sep;
    }

    (void)outError;
    return true;
}

// ---- ImGui：上下文 + CJK 字体 + 平台/渲染后端 ----
bool GlfwHost::initImGui(bool useVulkan) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::StyleColorsDark();

    if (!useVulkan) {
        ImGui_ImplGlfw_InitForOpenGL(_window, true);
        ImGui_ImplOpenGL3_Init("#version 150");
    }

    // 中文字体支持：默认 ProggyClean 无 CJK 字形。macOS 的 PingFang.ttc
    // 是 CFF 轮廓（stb_truetype 不支持），必须用 TrueType 轮廓的中文字体
    {
        const char* fontCandidates[] = {
#if defined(__APPLE__)
            "/System/Library/Fonts/STHeiti Medium.ttc",
            "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
            "/Library/Fonts/Arial Unicode.ttf",
#elif defined(_WIN32)
            "C:/Windows/Fonts/msyh.ttc",
            "C:/Windows/Fonts/msyh.ttf",
            "C:/Windows/Fonts/simhei.ttf",
            "C:/Windows/Fonts/simsun.ttc",
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
                    // 全 CJK 范围：GetGlyphRangesChineseSimplifiedCommon 只有 2500
                    // 常用字，"渲"/"帧"等字缺失会显示成 ?。全范围代价是图集略大，
                    // 对 demo 无所谓。
                    ImGui::GetIO().Fonts->GetGlyphRangesChineseFull());
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

#if ENGINE_HAS_VULKAN
    if (useVulkan) {
        auto* dev = RenderSystem::getInstance().rhiDevice();
        rhi::IRHIDevice::NativeHandles h;
        if (!dev || !dev->nativeHandles(h)) {
            std::cout << "VK: no native handles for ImGui" << std::endl;
            return false;
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
        if (vkCreateDescriptorPool((VkDevice)h.device, &pi, nullptr, &_imguiDescriptorPool) != VK_SUCCESS) {
            std::cout << "Failed to create ImGui descriptor pool" << std::endl;
            return false;
        }
        ImGui_ImplGlfw_InitForVulkan(_window, true);
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
        init.DescriptorPool = _imguiDescriptorPool;
        init.MinImageCount = h.minImageCount;
        init.ImageCount = h.imageCount;
        init.UseDynamicRendering = true;
        // V3-15：MSAA——ImGui 管线的采样数必须与主帧缓冲一致。
        // 引擎从 NativeHandles 暴露实际值（1 = 关闭），宿主不自己猜。
        init.MSAASamples = (VkSampleCountFlagBits)(h.msaaSamples ? h.msaaSamples : 1);
        init.PipelineRenderingCreateInfo = {};
        init.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
        init.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        init.PipelineRenderingCreateInfo.pColorAttachmentFormats = &s_imguiColorFormat;
        init.PipelineRenderingCreateInfo.depthAttachmentFormat = (VkFormat)h.depthFormat;
        init.CheckVkResultFn = [](VkResult err) {
            if (err != VK_SUCCESS) std::printf("[IMGUI-VK] CheckVkResult %d\n", (int)err);
        };
        ImGui_ImplVulkan_Init(&init);

        // B3c：VK 交换链为 sRGB，写回时硬件编码。样式色是 sRGB 作者值，
        // 直接写会被二次编码（UI 发白）→ 预转线性
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
        return true;
    }
#endif
    // GL：UI 在 renderFrame 的 overlay 合成点绘制（引擎已关 GL_FRAMEBUFFER_SRGB）
    RenderSystem::getInstance().setOverlayRenderer([](rhi::IRHICommandList*) {
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    });
    return true;
}

void GlfwHost::newFrameImGui(bool useVulkan) {
#if ENGINE_HAS_VULKAN
    if (useVulkan) {
        ImGui_ImplGlfw_NewFrame();      // 先设置 DisplaySize（依赖 GLFW 窗口）
        ImGui_ImplVulkan_NewFrame();
        return;
    }
#endif
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
}

void GlfwHost::shutdownImGui(bool useVulkan) {
#if ENGINE_HAS_VULKAN
    if (useVulkan) {
        // ImGui 的资源被最后一帧的命令缓冲引用，销毁前必须等 GPU 完成
        if (auto* dev = RenderSystem::getInstance().rhiDevice()) dev->waitIdle();
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        if (_imguiDescriptorPool != VK_NULL_HANDLE) {
            auto* dev = RenderSystem::getInstance().rhiDevice();
            rhi::IRHIDevice::NativeHandles h;
            if (dev && dev->nativeHandles(h)) {
                vkDestroyDescriptorPool((VkDevice)h.device, _imguiDescriptorPool, nullptr);
                _imguiDescriptorPool = VK_NULL_HANDLE;
            }
        }
        return;
    }
#endif
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
}

// ---- 窗口事件回调转发到 RenderSystem ----
void GlfwHost::framebufferSizeCallback(GLFWwindow*, int width, int height) {
    RenderSystem::getInstance().onWindowSizeChanged(width, height);
}

void GlfwHost::mouseCallback(GLFWwindow*, double xpos, double ypos) {
    if (ImGui::GetIO().WantCaptureMouse) return;   // 鼠标在 UI 上，不透传给场景
    RenderSystem::getInstance().onMouseMove(xpos, ypos);
}

void GlfwHost::scrollCallback(GLFWwindow*, double xoffset, double yoffset) {
    if (ImGui::GetIO().WantCaptureMouse) return;   // 鼠标在 UI 上，不透传给场景
    RenderSystem::getInstance().onMouseMiddleScroll(xoffset, yoffset);
}

void GlfwHost::keyCallback(GLFWwindow* window, int key, int scancode, int action, int mods) {
    (void)scancode; (void)mods;
    if (action == GLFW_PRESS && key == GLFW_KEY_ESCAPE) {
        glfwSetWindowShouldClose(window, GLFW_TRUE);
    }
}

void GlfwHost::mouseButtonCallback(GLFWwindow* window, int button, int action, int mods) {
    (void)mods;
    // ImGui 捕获鼠标（点菜单栏/面板）时，不透传给场景拾取/相机
    if (ImGui::GetIO().WantCaptureMouse) return;
    double xpos, ypos;
    glfwGetCursorPos(window, &xpos, &ypos);
    auto& rs = RenderSystem::getInstance();
    if (button == GLFW_MOUSE_BUTTON_LEFT) {
        if (action == GLFW_RELEASE) rs.onMouseLeftUp(xpos, ypos);
        else if (action == GLFW_PRESS) rs.onMouseLeftDown(xpos, ypos);
    } else if (button == GLFW_MOUSE_BUTTON_RIGHT) {
        if (action == GLFW_RELEASE) rs.onMouseRightUp(xpos, ypos);
        else if (action == GLFW_PRESS) rs.onMouseRightDown(xpos, ypos);
    } else if (button == GLFW_MOUSE_BUTTON_MIDDLE && action == GLFW_PRESS) {
        rs.onMouseMiddleDown(xpos, ypos);
    }
}
