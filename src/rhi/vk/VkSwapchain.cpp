//
//  VkSwapchain.cpp
//
#include "VkSwapchain.h"
#include <algorithm>
#include <cstdio>

namespace rhi {

bool VkSwapchain::create(VkPhysicalDevice pd, VkDevice device, VkSurfaceKHR surface,
                         uint32_t fbWidth, uint32_t fbHeight, VkFormat depthFmt,
                         uint32_t msaaSamples, VkMemoryAllocator* alloc, NameFn nameFn) {
    destroy();
    _pd     = pd;
    _device = device;
    _alloc  = alloc;
    _nameFn = std::move(nameFn);
    if (!_nameFn) _nameFn = [](VkObjectType, uint64_t, const char*) {};

    // C1a：RHI 不回查窗口系统——尺寸来自 DeviceDesc/onResize（宿主推送）
    const uint32_t fbW = fbWidth;
    const uint32_t fbH = fbHeight;

    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &caps);
    _extent = caps.currentExtent;
    if (_extent.width == UINT32_MAX) {
        _extent.width  = std::clamp(fbW, caps.minImageExtent.width,  caps.maxImageExtent.width);
        _extent.height = std::clamp(fbH, caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    // surface 格式（B3c）：优先 sRGB 交换链（线性工作流的编码端）。
    // 优先级：BGRA8_SRGB > RGBA8_SRGB > BGRA8_UNORM > RGBA8_UNORM > 驱动首选项。
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &fmtCount, formats.data());
    if (formats.empty()) {
        ENGINE_LOG_ERROR("[VK] surface exposes no formats");
        return false;
    }
    auto formatRank = [](VkFormat f) {
        switch (f) {
            case VK_FORMAT_B8G8R8A8_SRGB:  return 4;
            case VK_FORMAT_R8G8B8A8_SRGB:  return 3;
            case VK_FORMAT_B8G8R8A8_UNORM: return 2;
            case VK_FORMAT_R8G8B8A8_UNORM: return 1;
            default: return 0;
        }
    };
    // SRGB_NONLINEAR 是显示器的标准编码空间；只在它提供的格式对里挑选，
    // 没有任何匹配时退回驱动首选项（format/colorSpace 始终成对取自 surface 列表）。
    bool haveStandard = false;
    int bestRank = 0;
    for (const auto& f : formats) {
        if (f.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        const int r = formatRank(f.format);
        if (!haveStandard || r > bestRank) {
            bestRank    = r;
            _format     = f.format;
            _colorSpace = f.colorSpace;
            haveStandard = true;
        }
    }
    if (!haveStandard) {
        _format     = formats[0].format;
        _colorSpace = formats[0].colorSpace;
    }
    const bool swapchainSrgb = (_format == VK_FORMAT_B8G8R8A8_SRGB ||
                                _format == VK_FORMAT_R8G8B8A8_SRGB);
    ENGINE_LOG_INFO("[VK] swapchain format=%d colorSpace=%d sRGB=%d",
                    static_cast<int>(_format), static_cast<int>(_colorSpace),
                    swapchainSrgb ? 1 : 0);
    if (!swapchainSrgb) {
        ENGINE_LOG_WARN("[VK] no sRGB surface format; linear workflow will look different from GL");
    }

    uint32_t imageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface          = surface;
    sci.minImageCount    = imageCount;
    sci.imageFormat      = _format;
    sci.imageColorSpace  = _colorSpace;
    sci.imageExtent      = _extent;
    sci.imageArrayLayers = 1;
    // TRANSFER_SRC 供 RHI_PROBE 回读验证
    sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;   // vsync（M3 固定）
    sci.clipped          = VK_TRUE;
    if (vkCreateSwapchainKHR(device, &sci, nullptr, &_swapchain) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] vkCreateSwapchainKHR failed");
        _swapchain = VK_NULL_HANDLE;
        return false;
    }
    _nameFn(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<uint64_t>(_swapchain), "RHI_Swapchain");

    // image + view
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device, _swapchain, &n, nullptr);
    _images.resize(n);
    vkGetSwapchainImagesKHR(device, _swapchain, &n, _images.data());
    _views.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image    = _images[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = _format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vci, nullptr, &_views[i]) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] swapchain image view create failed");
            return false;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "RHI_SwapchainImage_%u", i);
        _nameFn(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(_images[i]), name);
        std::snprintf(name, sizeof(name), "RHI_SwapchainView_%u", i);
        _nameFn(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(_views[i]), name);
    }

    // per-image render-done 信号量（随交换链生命周期重建）
    _renderDone.resize(n, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo rsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; ++i) {
        if (vkCreateSemaphore(device, &rsci, nullptr, &_renderDone[i]) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] render-done semaphore create failed");
            return false;
        }
        char name[64];
        std::snprintf(name, sizeof(name), "RHI_RenderDone_%u", i);
        _nameFn(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(_renderDone[i]), name);
    }

    // dynamic rendering 无自动布局转换：acquire 后首帧是 UNDEFINED，
    // present 后由 recordPresentBarrier 置回 PRESENT_SRC
    _layouts.assign(n, VK_IMAGE_LAYOUT_UNDEFINED);

    // ---- 深度附件（单缓冲；frame-in-flight=1 下共享合法）----
    _depthFormat = depthFmt;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = _depthFormat;
    ici.extent        = {_extent.width, _extent.height, 1};
    ici.mipLevels     = 1;
    ici.arrayLayers   = 1;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ici, nullptr, &_depthImage) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] depth image create failed");
        return false;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, _depthImage, &req);
    _depthMemAlloc = _alloc->alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);   // E1
    if (!_depthMemAlloc.valid()) {
        ENGINE_LOG_ERROR("[VK] depth memory alloc failed");
        return false;
    }
    _depthMemory = _depthMemAlloc.memory;
    if (vkBindImageMemory(device, _depthImage, _depthMemory, _depthMemAlloc.offset) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] depth vkBindImageMemory failed");
        return false;
    }

    VkImageViewCreateInfo dvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    dvci.image    = _depthImage;
    dvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    dvci.format   = _depthFormat;
    dvci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &dvci, nullptr, &_depthView) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] depth image view create failed");
        return false;
    }
    _nameFn(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(_depthView), "RHI_DepthView");
    _nameFn(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(_depthImage), "RHI_DepthImage");

    _depthLayout   = VK_IMAGE_LAYOUT_UNDEFINED;

    // ---- MSAA 附件（msaaSamples > 1 时）----
    _msaaSamples = (msaaSamples > 1) ? msaaSamples : 1;
    if (_msaaSamples > 1 && !createMsaa()) {
        ENGINE_LOG_ERROR("[VK] MSAA attachments create failed (samples=%u)", _msaaSamples);
        return false;
    }

    _needsRecreate = false;
    return true;
}

// 多采样颜色/深度附件：大小与交换链一致，渲染时 resolve 进交换链图。
bool VkSwapchain::createMsaa() {
    VkSampleCountFlagBits samples = static_cast<VkSampleCountFlagBits>(_msaaSamples);

    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType     = VK_IMAGE_TYPE_2D;
    ci.extent        = {_extent.width, _extent.height, 1};
    ci.mipLevels     = 1;
    ci.arrayLayers   = 1;
    ci.samples       = samples;
    ci.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ci.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    // 颜色
    ci.format = _format;
    ci.usage  = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (vkCreateImage(_device, &ci, nullptr, &_msaaColorImage) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(_device, _msaaColorImage, &req);
    _msaaColorMem = _alloc->alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!_msaaColorMem.valid()) return false;
    if (vkBindImageMemory(_device, _msaaColorImage, _msaaColorMem.memory,
                          _msaaColorMem.offset) != VK_SUCCESS) return false;

    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image    = _msaaColorImage;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = _format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(_device, &vci, nullptr, &_msaaColorView) != VK_SUCCESS) return false;
    _nameFn(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(_msaaColorImage), "RHI_MsaaColor");

    // 深度
    ci.format = _depthFormat;
    ci.usage  = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (vkCreateImage(_device, &ci, nullptr, &_msaaDepthImage) != VK_SUCCESS) return false;
    vkGetImageMemoryRequirements(_device, _msaaDepthImage, &req);
    _msaaDepthMem = _alloc->alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!_msaaDepthMem.valid()) return false;
    if (vkBindImageMemory(_device, _msaaDepthImage, _msaaDepthMem.memory,
                          _msaaDepthMem.offset) != VK_SUCCESS) return false;

    vci.image  = _msaaDepthImage;
    vci.format = _depthFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(_device, &vci, nullptr, &_msaaDepthView) != VK_SUCCESS) return false;
    _nameFn(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(_msaaDepthImage), "RHI_MsaaDepth");

    _msaaColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    _msaaDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ENGINE_LOG_INFO("[VK] MSAA attachments created (samples=%u, %ux%u)",
                    _msaaSamples, _extent.width, _extent.height);
    return true;
}

void VkSwapchain::destroyMsaa() {
    if (_msaaColorView)  vkDestroyImageView(_device, _msaaColorView, nullptr);
    if (_msaaColorImage) vkDestroyImage(_device, _msaaColorImage, nullptr);
    if (_msaaColorMem.valid()) _alloc->free(_msaaColorMem);
    if (_msaaDepthView)  vkDestroyImageView(_device, _msaaDepthView, nullptr);
    if (_msaaDepthImage) vkDestroyImage(_device, _msaaDepthImage, nullptr);
    if (_msaaDepthMem.valid()) _alloc->free(_msaaDepthMem);
    _msaaColorView = VK_NULL_HANDLE; _msaaColorImage = VK_NULL_HANDLE;
    _msaaDepthView = VK_NULL_HANDLE; _msaaDepthImage = VK_NULL_HANDLE;
    _msaaColorMem = VkMemAlloc{}; _msaaDepthMem = VkMemAlloc{};
    _msaaColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    _msaaDepthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

// 每帧进 Pass 前建访问链：首帧从 UNDEFINED 转入附件布局，之后同布局链式访问。
void VkSwapchain::recordMsaaBeginBarriers(VkCommandBuffer cb) {
    if (_msaaColorImage == VK_NULL_HANDLE) return;

    VkImageMemoryBarrier color{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    color.srcAccessMask = (_msaaColorLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                              ? 0 : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    color.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    color.oldLayout = _msaaColorLayout;
    color.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    color.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    color.image = _msaaColorImage;
    color.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &color);
    _msaaColorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkImageMemoryBarrier depth{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    depth.srcAccessMask = (_msaaDepthLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                              ? 0 : VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                        | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    depth.oldLayout = _msaaDepthLayout;
    depth.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depth.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    depth.image = _msaaDepthImage;
    depth.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                         VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                         | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &depth);
    _msaaDepthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

void VkSwapchain::destroy() {
    if (_device == VK_NULL_HANDLE) return;
    destroyMsaa();
    for (auto s : _renderDone) if (s) vkDestroySemaphore(_device, s, nullptr);
    _renderDone.clear();
    for (auto v : _views)      if (v) vkDestroyImageView(_device, v, nullptr);
    if (_depthView)   vkDestroyImageView(_device, _depthView, nullptr);
    if (_depthImage)  vkDestroyImage(_device, _depthImage, nullptr);
    if (_depthMemory) {
        _alloc->free(_depthMemAlloc);   // E1：分配器负责 vkFreeMemory
        _depthMemory = VK_NULL_HANDLE;
    }
    _depthView = VK_NULL_HANDLE;
    _depthImage = VK_NULL_HANDLE;
    if (_swapchain) vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    _swapchain = VK_NULL_HANDLE;
    _layouts.clear();
    _views.clear();
    _images.clear();
    _extent = {0, 0};
    _format = VK_FORMAT_UNDEFINED;
    _depthFormat = VK_FORMAT_UNDEFINED;
    _depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
}

bool VkSwapchain::acquire(VkSemaphore imageAvailable, uint32_t& imageIndex) {
    if (!_swapchain) return false;
    VkResult r = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX,
                                       imageAvailable, VK_NULL_HANDLE, &imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        _needsRecreate = true;
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        ENGINE_LOG_ERROR("[VK] vkAcquireNextImageKHR failed (%d)", static_cast<int>(r));
        return false;
    }
    return true;
}

bool VkSwapchain::present(VkQueue queue, uint32_t imageIndex, VkSemaphore renderDone) {
    if (!_swapchain) return false;
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &renderDone;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &_swapchain;
    pi.pImageIndices      = &imageIndex;
    VkResult r = vkQueuePresentKHR(queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        _needsRecreate = true;
    }
    return r == VK_SUCCESS;
}

void VkSwapchain::recordBeginPassBarriers(VkCommandBuffer cb, uint32_t imageIndex,
                                          bool clearColor, bool clearDepth) {
    if (imageIndex >= _layouts.size()) return;
    VkImageLayout colorOld = _layouts[imageIndex];
    (void)clearColor;   // 布局切换只看当前布局；clear 由 loadOp 决定
    // 首帧（UNDEFINED）或上一帧呈现后（PRESENT_SRC）都要转到附件布局
    if (colorOld != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.oldLayout = colorOld;
        bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = _images[imageIndex];
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &bar);
        _layouts[imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (_depthLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || clearDepth) {
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                          | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bar.oldLayout = _depthLayout;   // UNDEFINED 首帧 → 一次性转换；之后同布局访问链
        bar.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = _depthImage;
        bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &bar);
        _depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
}

// B1：present 前把当前 image 转回 PRESENT_SRC（仅当本帧渲染过）。
// 本帧未渲染（acquire 后跳过，layout 仍 UNDEFINED）也必须转换——present 要求 PRESENT_SRC。
void VkSwapchain::recordPresentBarrier(VkCommandBuffer cb, uint32_t imageIndex) {
    if (imageIndex >= _layouts.size()) return;
    const VkImageLayout cur = _layouts[imageIndex];
    if (cur != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        && cur != VK_IMAGE_LAYOUT_UNDEFINED) return;
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcAccessMask = (cur == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
                            ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0;
    bar.dstAccessMask = 0;
    bar.oldLayout = cur;
    bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = _images[imageIndex];
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &bar);
    _layouts[imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

} // namespace rhi
