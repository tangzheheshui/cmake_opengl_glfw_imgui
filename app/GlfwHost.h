//
//  GlfwHost.h
//  app 层宿主：窗口 / GL 上下文 / RHI 设备创建 / ImGui 后端
//
//  C5（docs/ARCHITECTURE_INTERFACES.md §4.4）：引擎是库，GLFW 只是本示例
//  app 的窗口来源。宿主职责（§3.1）：
//    - 创建窗口与 GL 上下文（GL 后端要求 current）
//    - 填 DeviceDesc / NativeWindow（交换回调、VK surface 回调）
//    - 创建并持有 RHI 设备（引擎借用）
//    - ImGui 平台/渲染后端初始化与关闭
//    - 窗口事件回调转发到 RenderSystem
//
#ifndef GlfwHost_h
#define GlfwHost_h

#include <memory>
#include "rhi/RhiTypes.h"
#include <string>

#ifndef ENGINE_HAS_VULKAN
#define ENGINE_HAS_VULKAN 0
#endif
// GLFW_INCLUDE_NONE：GL 头由 glad 提供（glad 必须先于任何 GL 声明加载）；
// GLFW_INCLUDE_VULKAN：宿主 surface 回调需要 glfwCreateWindowSurface（VK 构建）
#define GLFW_INCLUDE_NONE
#if ENGINE_HAS_VULKAN
#define GLFW_INCLUDE_VULKAN
#endif
#include <GLFW/glfw3.h>
#if ENGINE_HAS_VULKAN
#include <vulkan/vulkan.h>
#endif

namespace rhi { class IRHIDevice; }

class GlfwHost {
public:
    GlfwHost() = default;
    ~GlfwHost();

    GlfwHost(const GlfwHost&) = delete;
    GlfwHost& operator=(const GlfwHost&) = delete;

    // 创建窗口（按后端选择 GL 上下文 / NO_API）。GL 模式下加载 glad 并把
    // 上下文设为 current。失败返回 false（窗口创建失败时内部已 terminate）。
    bool createWindow(int width, int height, bool useVulkan, std::string* outError = nullptr);

    // 组装 DeviceDesc 交给 RenderSystem（设备由引擎自建自毁，app 不再持有）
    bool buildDeviceDesc(bool useVulkan, std::string* outError = nullptr);

    // ---- ImGui（UI 是 app 层职责，含 CJK 字体加载）----
    bool initImGui(bool useVulkan);
    void newFrameImGui(bool useVulkan);   // backend NewFrame（每帧循环开头）
    void shutdownImGui(bool useVulkan);

    GLFWwindow* window() const { return _window; }
    // 设备生命周期归 RenderSystem（见 InitDesc 两种所有权模式）
    const rhi::DeviceDesc& deviceDesc() const { return _deviceDesc; }

private:
    static void framebufferSizeCallback(GLFWwindow* w, int width, int height);
    static void mouseCallback(GLFWwindow* w, double xpos, double ypos);
    static void scrollCallback(GLFWwindow* w, double xoffset, double yoffset);
    static void keyCallback(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void mouseButtonCallback(GLFWwindow* w, int button, int action, int mods);
    // GL 交换回调（RHI present() 转发；引擎不直接碰 GLFW）
    static void swapAdapter(void* user);

    GLFWwindow* _window = nullptr;
    rhi::DeviceDesc _deviceDesc;
#if ENGINE_HAS_VULKAN
    VkDescriptorPool _imguiDescriptorPool = VK_NULL_HANDLE;
#endif
};

#endif /* GlfwHost_h */
