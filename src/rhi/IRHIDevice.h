//
//  IRHIDevice.h
//  RHI 抽象层 - 设备接口（资源创建 + 帧循环控制）
//
#ifndef IRHIDevice_h
#define IRHIDevice_h

#include "RhiTypes.h"
#include <memory>

struct GLFWwindow;

namespace rhi {

class IRHICommandList;

class IRHIDevice {
public:
    virtual ~IRHIDevice() = default;

    virtual Backend    backend() const = 0;
    virtual DeviceInfo info() const = 0;

    // ---- 资源生命周期 ----
    // v1：句柄由后端内部表管理；对象级销毁在 M2 补齐（当前随 device 析构统一回收）。
    virtual BufferHandle   createBuffer(const BufferDesc& desc) = 0;
    virtual TextureHandle  createTexture(const TextureDesc& desc) = 0;
    virtual PipelineHandle createPipeline(const GraphicsPipelineDesc& desc) = 0;

    // 运行期更新 buffer 内容（data 区间 [offset, offset+size) 必须 ≤ 创建时的大小）
    // GL: glBufferSubData；Vulkan: staging 拷贝或 HOST_VISIBLE 直写
    virtual void updateBuffer(BufferHandle buffer, const void* data,
                              uint32_t size, uint32_t offset = 0) = 0;

    // 绘制级 uniform 块：每个绘制对象独立一块（避免共享 UBO 的逐帧覆盖问题）。
    // GL 后端返回哑句柄（数据经 setDrawUniforms 按名直传）；Vulkan 后端为
    // HOST_VISIBLE UBO + 独立描述符集（binding0=UBO，1..4=白色兜底采样器）。
    virtual UniformBlockHandle createUniformBlock(uint32_t size) = 0;

    // ---- 帧循环 ----
    // GL 后端：空实现（swap 由 main 里的 glfwSwapBuffers 完成，保持现有主循环不变）
    // Vulkan 后端：beginFrame = acquire image；endFrame = submit + present
    virtual void beginFrame() = 0;
    virtual void endFrame()   = 0;

    // 窗口尺寸变化通知（Vulkan 后端重建 swapchain；GL 后端 no-op）
    virtual void onResize(int width, int height) { (void)width; (void)height; }

    // 命令录制（每帧从 device 获取，框架内部可复用同一实例）
    virtual IRHICommandList* commandList() = 0;

    // ---- 原生句柄（app 层接入第三方库用，如 ImGui Vulkan backend）----
    struct NativeHandles {
        uint64_t instance       = 0;   // VkInstance
        uint64_t physicalDevice = 0;   // VkPhysicalDevice
        uint64_t device         = 0;   // VkDevice
        uint64_t queue          = 0;   // VkQueue
        uint64_t renderPass     = 0;   // VkRenderPass（默认帧缓冲 pass）
        uint32_t queueFamily    = 0;
        uint32_t minImageCount  = 2;
        uint32_t imageCount     = 2;
    };
    virtual bool nativeHandles(NativeHandles& out) const { (void)out; return false; }

    // 等待 GPU 完成（调试/销毁前用；GL: glFinish；Vulkan: vkDeviceWaitIdle）
    virtual void waitIdle() = 0;

    // ---- 逃生舱 ----
    // 返回后端原生句柄：GL → GLuint；Vulkan → VkBuffer/VkImage/VkPipeline。
    // 用于过渡期与旧 GL 代码互操作（如把现有阴影 FBO 直通给 RenderPassDesc）。
    // 注意：这层是有意留的"后门"，正式路径不要依赖它。
    virtual uint64_t nativeHandle(RhiHandle handle) const = 0;
};

} // namespace rhi

#endif /* IRHIDevice_h */
