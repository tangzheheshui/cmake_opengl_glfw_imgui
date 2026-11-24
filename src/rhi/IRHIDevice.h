//
//  IRHIDevice.h
//  RHI 抽象层 - 设备接口（资源创建 + 帧循环控制）
//
#ifndef IRHIDevice_h
#define IRHIDevice_h

#include "RhiTypes.h"
#include <cstddef>
#include <memory>
#include <vector>

namespace rhi {

class IRHICommandList;

class IRHIDevice {
public:
    virtual ~IRHIDevice() = default;

    virtual Backend    backend() const = 0;
    virtual DeviceInfo info() const = 0;

    // 结构化设备能力（调试面板展示 + 启动日志 + 上层路径选择）。
    // GL 后端在 B2 与 Vulkan 对齐；默认空表（0/false = 未知）。
    virtual DeviceCapabilities capabilities() const { return {}; }

    // ---- v2 对象级销毁（B0 冻结）----
    // 句柄 id 不复用：销毁后槽位保留为失效，访问返回空对象。
    // GL/VK 均在 B0 实现 buffer/texture/pipeline；uniform block 随 device 回收（E1 细化）。
    virtual void destroyBuffer(BufferHandle) {}
    virtual void destroyTexture(TextureHandle) {}
    virtual void destroyPipeline(PipelineHandle) {}
    virtual void destroyUniformBlock(UniformBlockHandle) {}

    // ---- V3-1：句柄有效性查询 ----
    // 默认只做 valid()（id != 0）；后端接入 HandleTable 后覆写，
    // 增加 id 范围 + gen 代际校验，访问已销毁/越界句柄返回 false 而非静默误用。
    virtual bool alive(BufferHandle h)   const { return h.valid(); }
    virtual bool alive(TextureHandle h)  const { return h.valid(); }
    virtual bool alive(PipelineHandle h) const { return h.valid(); }
    virtual bool alive(SamplerHandle h)  const { return h.valid(); }

    // ---- v2 采样器（B2 GL / B3 VK 生效；默认不支持）----
    virtual SamplerHandle createSampler(const SamplerDesc&) { return {}; }
    virtual void destroySampler(SamplerHandle) {}

    // ---- v2 离屏渲染目标（B4 实现；默认不支持）----
    virtual RenderTargetHandle createRenderTarget(const RenderTargetDesc&) { return {}; }
    virtual void destroyRenderTarget(RenderTargetHandle) {}
    virtual TextureHandle renderTargetColorTexture(RenderTargetHandle) const { return {}; }

    // ---- v2 同步回读（B4 实现；outSize 必须 ≥ 区域字节数）----
    virtual bool readbackTexture(TextureHandle, void* /*outData*/, size_t /*outSize*/,
                                 const ReadbackDesc& = {}) { return false; }

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
    // GL 后端：执行延迟销毁队列（V3-0 DeletionQueue），恒返回 true
    // Vulkan 后端：beginFrame = 等待 fence + acquire image；endFrame = submit + present
    // 返回 false = 本帧无法开始（swapchain 过期重建中/窗口最小化），上层必须
    // 跳过全部命令录制（此前返回 void 时失败被内部吞掉，上层会录制一个
    // 不会提交的帧——V3-0 止血项）
    virtual bool beginFrame() = 0;
    virtual void endFrame()   = 0;

    // 窗口尺寸变化通知（Vulkan 后端重建 swapchain；GL 后端保存尺寸）
    virtual void onResize(int width, int height) { (void)width; (void)height; }

    // ---- C1a：尺寸查询与呈现（RHI 永不回查窗口系统）----
    struct FramebufferSize {
        uint32_t width  = 0;
        uint32_t height = 0;
    };
    // 默认帧缓冲/离屏目标的像素尺寸（GL：构造/onResize 保存的值；
    // VK：swapchain extent）
    virtual FramebufferSize framebufferSize() const { return {}; }

    // 呈现当前帧。GL：调用 NativeWindow::swapBuffers 回调（宿主拥有窗口）；
    // VK：vkQueuePresentKHR；离屏：no-op。
    // C1b 起由 RenderSystem::renderFrame 统一调用。
    virtual void present() {}

    // 命令录制（每帧从 device 获取，框架内部可复用同一实例）
    virtual IRHICommandList* commandList() = 0;

    // ---- 原生句柄（app 层接入第三方库用，如 ImGui Vulkan backend）----
    struct NativeHandles {
        uint64_t instance       = 0;   // VkInstance
        uint64_t physicalDevice = 0;   // VkPhysicalDevice
        uint64_t device         = 0;   // VkDevice
        uint64_t queue          = 0;   // VkQueue
        uint64_t renderPass     = 0;   // B1 后恒为 0（dynamic rendering；ImGui 用 UseDynamicRendering）
        uint32_t queueFamily    = 0;
        uint32_t minImageCount  = 2;
        uint32_t imageCount     = 2;
        // B1 追加（契约"只加不改"）：默认渲染目标附件格式（VkFormat 原始值），
        // 供宿主侧构建 PipelineRenderingCreateInfo / 兼容附件
        uint32_t colorFormat    = 0;   // swapchain 图像格式（VkFormat）
        uint32_t depthFormat    = 0;   // 深度附件格式（VkFormat）
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
