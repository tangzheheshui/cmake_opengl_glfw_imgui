//
//  main.cpp —— 示例宿主（C5：启动编排 → 主循环 → 关闭，无场景/资源细节）
//
//  宿主职责（docs/ARCHITECTURE_INTERFACES.md §3.1）：
//    窗口与 GL 上下文（GlfwHost）→ 设备创建（RHIFactory + DeviceDesc）
//    → RenderSystem::init（借用设备）→ 每帧 renderFrame() → 显式关闭
//
//  生命周期约定（C1b）：
//    窗口创建 → 设备创建 → renderer.init → 每帧 renderer.renderFrame()
//    → renderer.shutdown() → 设备销毁 → 窗口销毁（glfwTerminate）
//
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>

#include "imgui.h"
#include "RenderSystem.h"
#include "app/GlfwHost.h"
#include "app/DebugPanel.h"
#include "app/SceneDemo.h"
#include "FontManager.h"
#include "rhi/IRHIDevice.h"
#include "fs/FilePathUtil.h"
#include "debug/CrashBacktrace.h"

#ifndef ENGINE_HAS_VULKAN
#define ENGINE_HAS_VULKAN 0
#endif

int main() {
    // RHI_DEBUG_LOG=1：stdout 无缓冲 + SIGABRT/SIGSEGV backtrace（崩溃定位手段）
    if (std::getenv("RHI_DEBUG_LOG")) {
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        engine::debug::installCrashBacktrace();
    }

    // ---- 后端选择（宿主职责；引擎不读环境变量）----
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

    // ---- 窗口 / GL 上下文 / 设备（GlfwHost）----
    GlfwHost host;
    std::string err;
    if (!host.createWindow(1600, 900, useVulkan, &err)) {
        std::cout << "Failed to create window: " << err << std::endl;
        glfwTerminate();
        return -1;
    }
    if (!host.createDevice(useVulkan, &err)) {
        std::cout << "Failed to create RHI device: " << err << std::endl;
        glfwTerminate();
        return -1;
    }

    // ---- 引擎初始化（借用设备）----
    int fbW = 0, fbH = 0, winW = 0, winH = 0;
    glfwGetFramebufferSize(host.window(), &fbW, &fbH);
    glfwGetWindowSize(host.window(), &winW, &winH);
    std::string proPath = engine::fs::utf8FromPath(std::filesystem::current_path());
    {
        const size_t pos = proPath.find("/build");
        if (pos != std::string::npos) proPath = proPath.substr(0, pos);
    }

    RenderSystem::InitDesc initDesc;
    initDesc.device       = host.device();
    initDesc.assetRoot    = proPath;
    initDesc.frameWidth   = static_cast<uint32_t>(fbW);
    initDesc.frameHeight  = static_cast<uint32_t>(fbH);
    initDesc.windowWidth  = static_cast<uint32_t>(winW);
    initDesc.windowHeight = static_cast<uint32_t>(winH);
    if (!RenderSystem::getInstance().init(initDesc, &err)) {
        std::cout << "Failed to init RenderSystem: " << err << std::endl;
        glfwTerminate();
        return -1;
    }
    RenderSystem::getInstance().onWindowSizeChanged(fbW, fbH);

    // 资源预载（app 层职责）+ 调试开关经 settings() 注入
    {
        auto& renderSys = RenderSystem::getInstance();
        renderSys.preloadTextures(engine::fs::utf8FromPath(
            engine::fs::pathFromUtf8(proPath) / "res"));
        if (!useVulkan) {
            renderSys.GetFontMng()->LoadFont(engine::fs::utf8FromPath(
                engine::fs::pathFromUtf8(proPath) / "res" / "font" / "arial.ttf"), 48);
        }
        if (std::getenv("RHI_SMOKE"))   renderSys.settings().rhiSmokeTriangle = true;
        if (std::getenv("RHI_TEXQUAD")) renderSys.settings().rhiTexQuad = true;
        if (const char* clip = std::getenv("RHI_CLIP")) {   // D7 剖切（法线 (1,0,0)）
            renderSys.settings().clipPlaneEnabled = true;
            renderSys.settings().clipPlaneDistance = static_cast<float>(std::atof(clip));
        }
    }

    // ---- ImGui（app 层 UI）----
    if (!host.initImGui(useVulkan)) {
        RenderSystem::getInstance().shutdown();
        glfwTerminate();
        return -1;
    }

    // ---- 场景组装（app 层示例内容）----
    SceneDemo::buildScene(proPath, useVulkan);

    // ---- 主循环：UI 帧 → renderFrame（内部 overlay 合成 + present）→ 事件 ----
    long exitAfterFrames = -1;
    if (const char* e = std::getenv("RHI_EXIT_AFTER")) exitAfterFrames = std::atol(e);
    // RHI_RESIZE_AT=<帧>：指定帧改变窗口尺寸（swapchain 重建回归）
    long resizeAtFrame = -1;
    if (const char* e = std::getenv("RHI_RESIZE_AT")) resizeAtFrame = std::atol(e);
    long frameIndex = 0;

    auto& renderSys = RenderSystem::getInstance();
    while (!glfwWindowShouldClose(host.window())) {
        if (exitAfterFrames >= 0 && ++frameIndex >= exitAfterFrames) {
            glfwSetWindowShouldClose(host.window(), GLFW_TRUE);
        }
        if (resizeAtFrame > 0 && frameIndex == resizeAtFrame) {
            std::printf("[UI] resize test: window -> 1100x700 at frame %ld\n", frameIndex);
            std::fflush(stdout);
            glfwSetWindowSize(host.window(), 1100, 700);
        }

        renderSys.update();

        host.newFrameImGui(useVulkan);
        ImGui::NewFrame();
        DebugPanel::draw(renderSys.settings(), renderSys.rhiDevice());
        ImGui::Render();

        renderSys.renderFrame();   // beginFrame → passes → overlay(UI) → endFrame → present

        glfwPollEvents();
    }

    // ---- 关闭（顺序即契约）：UI → 引擎 → 设备 → 窗口 ----
    host.shutdownImGui(useVulkan);
    renderSys.shutdown();          // 只销毁引擎资源，不销毁设备
    host.takeDevice().reset();     // 宿主销毁设备（GL 上下文/窗口仍存活）
    glfwTerminate();
    return 0;
}
