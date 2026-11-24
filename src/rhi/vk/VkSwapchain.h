//
//  VkSwapchain.h
//  RHI Vulkan 后端 —— 交换链（V3-3 从 VulkanDevice 拆出）
//
//  归属的东西：
//    · swapchain + 每张 image 的 view、布局跟踪
//    · 深度附件（image/view/布局）
//    · 每张 image 一个 render-done 信号量（present 等待用）
//
//  dynamic rendering 下没有 renderpass/framebuffer，随交换链重建的只有这些。
//  设备只保留帧状态（当前 image 下标、是否重建），具体资源都在这里。
//
#ifndef VkSwapchain_h
#define VkSwapchain_h

#include "VkCommon.h"
#include <functional>
#include <vector>

namespace rhi {

class VkSwapchain {
public:
    using NameFn = std::function<void(VkObjectType, uint64_t, const char*)>;

    // 创建（已存在时先销毁再建）；失败返回 false，调用方按致命处理
    bool create(VkPhysicalDevice pd, VkDevice device, VkSurfaceKHR surface,
                uint32_t fbWidth, uint32_t fbHeight, VkFormat depthFmt,
                VkMemoryAllocator* alloc, NameFn nameFn);
    void destroy();

    // 取下一张可渲染的 image。OUT_OF_DATE 时置重建标志并返回 false。
    bool acquire(VkSemaphore imageAvailable, uint32_t& imageIndex);
    // 呈现（等待该 image 的 render-done）。OUT_OF_DATE/SUBOPTIMAL 置重建标志。
    bool present(VkQueue queue, uint32_t imageIndex, VkSemaphore renderDone);

    // ---- 查询 ----
    bool     valid() const      { return _swapchain != VK_NULL_HANDLE; }
    VkExtent2D extent() const   { return _extent; }
    VkFormat format() const     { return _format; }
    VkFormat depthFormat() const{ return _depthFormat; }
    uint32_t imageCount() const { return static_cast<uint32_t>(_images.size()); }
    VkImageView colorView(uint32_t i) const {
        return i < _views.size() ? _views[i] : VK_NULL_HANDLE;
    }
    VkImage image(uint32_t i) const {
        return i < _images.size() ? _images[i] : VK_NULL_HANDLE;
    }
    VkImageView depthView() const { return _depthView; }
    VkSemaphore renderDoneSemaphore(uint32_t i) const {
        return i < _renderDone.size() ? _renderDone[i] : VK_NULL_HANDLE;
    }

    // ---- Pass 边界布局转换（dynamic rendering 无自动转换）----
    void recordBeginPassBarriers(VkCommandBuffer cb, uint32_t imageIndex,
                                 bool clearColor, bool clearDepth);
    void recordPresentBarrier(VkCommandBuffer cb, uint32_t imageIndex);

    // ---- 重建标志 ----
    bool needsRecreate() const { return _needsRecreate; }
    void requestRecreate()     { _needsRecreate = true; }

private:
    VkDevice       _device = VK_NULL_HANDLE;
    VkPhysicalDevice _pd   = VK_NULL_HANDLE;
    VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
    VkFormat       _format    = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR _colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D     _extent    = {0, 0};

    std::vector<VkImage>       _images;
    std::vector<VkImageView>   _views;
    std::vector<VkImageLayout> _layouts;
    std::vector<VkSemaphore>   _renderDone;

    VkFormat       _depthFormat = VK_FORMAT_UNDEFINED;
    VkImage        _depthImage  = VK_NULL_HANDLE;
    VkDeviceMemory _depthMemory = VK_NULL_HANDLE;
    VkMemAlloc     _depthMemAlloc;
    VkImageView    _depthView   = VK_NULL_HANDLE;
    VkImageLayout  _depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkMemoryAllocator* _alloc = nullptr;
    NameFn _nameFn;
    bool   _needsRecreate = false;
};

} // namespace rhi

#endif /* VkSwapchain_h */
