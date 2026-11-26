//
//  VulkanDevice.cpp
//
#include "log/LogMacros.h"
#include "VkDevice.h"
#include "VkDeviceInternal.h"
#include "VkCommandList.h"
#include "rhi/FormatInfo.h"
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include "fs/FilePathUtil.h"
#include "VkShaderReflect.h"

// C1a：平台 surface 建面宏由 CMake 注入（VK_USE_PLATFORM_*）。公共头不出现
// Vk 类型；此处是全库唯一允许平台分支的位置（后端内部实现细节）。
#if defined(VK_USE_PLATFORM_WIN32_KHR)
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif
#if defined(VK_USE_PLATFORM_METAL_EXT)
#include <vulkan/vulkan_metal.h>
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
#include <X11/Xlib.h>
#include <vulkan/vulkan_xlib.h>
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
#include <wayland-client.h>
#include <vulkan/vulkan_wayland.h>
#endif

namespace rhi {

// C1a：按 NativeWindowType 推导实例扩展（宿主未提供 instanceExtensions 时）。
// 返回 false = 该窗口类型无对应平台 surface 扩展（无法自建面）。


// ---- 工具：读取 SPIR-V 文件 ----
// A4: 路径按 UTF-8 解释（Windows 上宽字符打开，中文安装路径可用）
// 深度附件格式：按支持度依次尝试

static uint32_t findMemoryType(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

// 完整 mip 链级数（TextureDesc::mipLevels == 0 的语义）

// blit 能力（mipmap 生成依赖；可选特性，运行时探测，不支持则回退单级）

// ============================================================
// 创建 / 销毁
// ============================================================
// C1a：由 DeviceDesc 构造。早期初始化失败写入 outError 并保持 _initialized=false
//（工厂转译为 nullptr + 诊断信息）；深度失败（资源路径 VK_CHECK）仍 abort，
// V3-1 引入 Result<T> 后收口。






// C1a：surface 来源优先级 externalSurface（宿主持有，RHI 不销毁）
// → createSurface 回调（RHI 接管）→ 平台自建（RHI 接管）。







// ============================================================
// A3: Pipeline Cache 持久化
// ============================================================
// 缓存文件头校验：跨驱动/跨设备的缓存可能不兼容。规范要求驱动忽略不兼容数据，
// 但主动校验可避免验证层告警，也能在日志里说明"为什么没加速"。
// ============================================================
// 帧循环
// ============================================================
bool VulkanDevice::beginFrameInternal() {
    // V3-4：等本槽位上一次提交完成（首帧 fence 预置已信号，直接通过）
    if (!_frames.wait()) {
        // 设备丢失时 fence 会直接失败；置标志停掉后续所有 GPU 工作
        _deviceLost = true;
        ENGINE_LOG_ERROR("[VK] DEVICE LOST (fence wait); rendering stopped, "
                         "host should recreate the device");
        return false;
    }
    if (!_swapchain.acquire(_frames.current().imageAvailable, _imageIndex)) {
        return false;   // OUT_OF_DATE 等：本帧跳过，beginFrame 下一轮重建
    }
    if (!_frames.resetFence()) {
        ENGINE_LOG_ERROR("[VK] frame fence reset failed");
        return false;
    }
    return true;
}

void VulkanDevice::deferGpuWork(std::function<void(VkCommandBuffer)> fn, std::function<void()> onComplete) {
    // E2：一律排队，由下一次 beginFrame 的 drainDeferredGpuWork 批量提交。
    // 以前帧外创建（初始化阶段）是"每张纹理 submit + waitIdle 一次"，加载几十张
    // 纹理就是几十次同步往返；批量后整批只提交一次、等一次。
    _deferredGpu.push_back({std::move(fn), std::move(onComplete)});
}

void VulkanDevice::drainDeferredGpuWork() {
    if (_deferredGpu.empty()) return;

    // E2：整批录进一个临时命令缓冲，一次提交、一次等待。
    // 用独立命令缓冲是为了不动主命令缓冲（此时它可能仍处于 pending 状态）。
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = _commandPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(_device, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    for (auto& w : _deferredGpu) {
        if (w.record) w.record(cb);
    }
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(_queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(_queue));

    // GPU 已执行完，完成回调（staging 释放等）现在安全
    for (auto& w : _deferredGpu) {
        if (w.onComplete) w.onComplete();
    }
    vkFreeCommandBuffers(_device, _commandPool, 1, &cb);

    ENGINE_LOG_INFO("[VK] upload batch: %zu item(s) in one submit", _deferredGpu.size());
    _deferredGpu.clear();
    // 句柄占位已随回调执行完毕（回调自己持有 shared_ptr），向量可以清空
    _deferredTexHandles.clear();
}

bool VulkanDevice::beginFrame() {
    drainDeferredGpuWork();   // 上一帧帧内延迟的纹理上传：现在帧外安全执行
    if (_swapchain.needsRecreate()) {
        vkDeviceWaitIdle(_device);
        destroySwapchainObjects();
        createSwapchainObjects();
        // B1：dynamic rendering 下管线不引用 renderPass/framebuffer，
        // swapchain 重建不再需要重建 PSO（原来 recreatePipelines 的唯一理由）
    }
    _frameSkipped = !beginFrameInternal();
    _frameActive  = !_frameSkipped;
    if (_frameActive) {
        // V3-0：fence 已等待 = 上一帧提交完成，此处的到期销毁安全。
        // 帧号先 flush 再递增（登记 base = 已完成的最近帧）。
        _delQueue.flush(_frameCount);
        ++_frameCount;

        // V3-2：帧内 uniform 环归零。上一帧命令已执行完（fence 已等待），
        // 现在整块复用是安全的；offset 0 留给"没调 setDrawUniforms 的绘制"。
        // 命令列表各自归零本槽位的 uniform 环并开始录制
        if (_commandList) _commandList->beginFrameSlot(_frames.index());
        for (auto& l : _extraLists) l->beginFrameSlot(_frames.index());
        for (auto& l : _secondaryLists) l->beginFrameSlot(_frames.index());
        multListSelfTest();
        commandListChurnTest();
        textureSlotChurnTest();
    }
    return _frameActive;
}

void VulkanDevice::endFrame() {
    if (!_frameActive) { _frameSkipped = false; return; }
    _frameActive = false;

    // B1：dynamic rendering 无 renderpass finalLayout，present 前手动转回 PRESENT_SRC。
    // 仅在本帧真正渲染过（布局已被转到 COLOR_ATTACHMENT）时需要；空帧已是 PRESENT。
    // V3-14：并发录制的工作线程必须在结束命令缓冲/提交之前 join
    if (_mlWorker.joinable()) _mlWorker.join();

    // V3-6：提交顺序 = 用过的附加列表（按创建顺序）→ 主列表。
    // 主列表最后执行，负责最终呈现屏障（UI 叠加也录在它上面）。
    std::vector<VkCommandBuffer> cbs;
    cbs.reserve(_extraLists.size() + 1);
    for (auto& l : _extraLists) {
        if (l->used()) cbs.push_back(l->commandBuffer());
    }
    VkCommandBuffer cb = _commandList ? _commandList->commandBuffer() : VK_NULL_HANDLE;
    if (cb == VK_NULL_HANDLE) return;
    if (_mlTest && _mlBuf) {
        vkCmdFillBuffer(cb, _mlBuf, 0, sizeof(uint32_t), 0xBBBBBBBBu);
    }
    // secondary 已在录制线程 endRenderPass 里 end 命令缓冲（execute 前必须 end）
    recordPresentBarrier(cb);
    VK_CHECK(vkEndCommandBuffer(cb));
    cbs.push_back(cb);
    for (auto& l : _extraLists) {
        if (l->used()) VK_CHECK(vkEndCommandBuffer(l->commandBuffer()));
    }

    // 每个 swapchain image 使用独立的 render-done semaphore：
    // presentation 完成前不能复用同一个 binary semaphore（VUID-...-00067）
    VkSemaphore renderDone = _swapchain.renderDoneSemaphore(_imageIndex);
    if (renderDone == VK_NULL_HANDLE) {
        ENGINE_LOG_ERROR("[VK] missing render-done semaphore for image %u", _imageIndex);
        return;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    VkSemaphore imageAvailable = _frames.current().imageAvailable;
    si.pWaitSemaphores    = &imageAvailable;
    si.pWaitDstStageMask  = &waitStage;
    si.commandBufferCount = static_cast<uint32_t>(cbs.size());
    si.pCommandBuffers    = cbs.data();
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &renderDone;
    {
        const VkResult submitResult = vkQueueSubmit(_queue, 1, &si, _frames.current().fence);
        if (submitResult == VK_ERROR_DEVICE_LOST) {
            _deviceLost = true;
            ENGINE_LOG_ERROR("[VK] DEVICE LOST (queue submit); rendering stopped");
            return;
        }
        VK_CHECK(submitResult);
    }
    _frameSubmitted = true;   // C1b：present() 消费该标志
    _frames.advance();        // 下一帧换槽位（本帧 GPU 未完成前不覆盖它）

    // ---- RHI_PROBE=1：第 30 帧回读中心像素（M3 验证用，之后可移除）----
    // 必须在 present 之前执行：此时图像仍处于 acquired 状态，布局转换合法。
    // 单帧调试路径，允许同步 vkQueueWaitIdle。
    static int frameCount = 0;
    static bool probeEnabled = (std::getenv("RHI_PROBE") != nullptr);
    if (probeEnabled && ++frameCount == 30) {
        vkQueueWaitIdle(_queue);   // 主提交完成；图像处于 PRESENT_SRC，仍 acquired
        VkBuffer readback;
        VkMemAlloc readbackMem;   // E1：子分配 + 页持久映射
        const VkExtent2D scExtent = _swapchain.extent();
        VkDeviceSize bufSize = scExtent.width * scExtent.height * 4;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size  = bufSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(_device, &bci, nullptr, &readback));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(_device, readback, &req);
        readbackMem = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                          | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkBindBufferMemory(_device, readback, readbackMem.memory, readbackMem.offset));

        // 主命令缓冲已完成，可 reset 后复用录制回读
        VkCommandBuffer cb = _commandList ? _commandList->commandBuffer() : VK_NULL_HANDLE;
        if (cb == VK_NULL_HANDLE) return;
        VK_CHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));
        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = _swapchain.image(_imageIndex);
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toTransfer);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {scExtent.width, scExtent.height, 1};
        vkCmdCopyImageToBuffer(cb, _swapchain.image(_imageIndex),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        // 拷贝后必须恢复 PRESENT_SRC，才能交给 present
        VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toPresent.dstAccessMask = 0;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = _swapchain.image(_imageIndex);
        toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toPresent);
        VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo s2{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s2.commandBufferCount = 1;
        s2.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(_queue, 1, &s2, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(_queue));

        // E1：页持久映射，直接取指针
        const uint8_t* px = static_cast<const uint8_t*>(readbackMem.mapped);
        uint32_t cx = scExtent.width / 2, cy = scExtent.height * 2 / 5;
        const size_t off = (static_cast<size_t>(cy) * scExtent.width + cx) * 4;
        // swapchain 格式可能是 BGRA：打印原始字节 + 两种解释
        ENGINE_LOG_INFO("[VKPROBE] center bytes = %d,%d,%d,%d (format=%d)",
                    px[off], px[off+1], px[off+2], px[off+3], (int)_swapchain.format());
        std::fflush(stdout);
        if (std::getenv("RHI_DUMP")) {   // 正式调试设施：导出整帧 PPM（跨平台临时目录）
            std::error_code ec;
            auto tmpDir = std::filesystem::temp_directory_path(ec);
            if (ec) tmpDir = ".";
            // A4: 保持 path 对象直接打开（临时目录可能含中文用户名，Windows ACP fopen 会失败）
            FILE* fp = engine::fs::openWrite(tmpDir / "frame_vk.ppm");
            if (fp) {
                std::fprintf(fp, "P6\n%u %u\n255\n", scExtent.width, scExtent.height);
                // B3a 完成后 3D 内容已是正立，按屏幕行序直接导出
                for (uint32_t y = 0; y < scExtent.height; ++y) {
                    const uint8_t* row = px + static_cast<size_t>(y) * scExtent.width * 4;
                    for (uint32_t x = 0; x < scExtent.width; ++x) {
                        const uint8_t* px4 = row + static_cast<size_t>(x) * 4;
                        const uint8_t rgb[3] = { px4[2], px4[1], px4[0] };   // BGRA → RGB 真色
                        fwrite(rgb, 1, 3, fp);
                    }
                }
                fclose(fp);
            }
        }
        vkDestroyBuffer(_device, readback, nullptr);
        _memAlloc.free(readbackMem);   // E1
        std::fflush(stdout);
    }
    // C1b：present 由 IRHIDevice::present() 独立执行（GL 宿主 swapBuffers 与
    // VK vkQueuePresentKHR 同一入口），endFrame 只负责 submit
}

// ---- C1a/C1b：呈现独立于提交（GL 宿主 swap 与 VK present 同一入口）----
void VulkanDevice::present() {
    if (!_frameSubmitted) return;   // 帧跳过/未提交：无可呈现图像
    _frameSubmitted = false;
    VkSemaphore renderDone = _swapchain.renderDoneSemaphore(_imageIndex);
    if (!renderDone) return;
    const VkResult r = _swapchain.present(_queue, _imageIndex, renderDone);
    if (r == VK_ERROR_DEVICE_LOST) {
        _deviceLost = true;
        ENGINE_LOG_ERROR("[VK] DEVICE LOST (present); rendering stopped");
    }
}

void VulkanDevice::onResize(int width, int height) {
    // C1a：尺寸由宿主推送，保存为帧缓冲尺寸真相源（framebufferSize 返回值）
    if (width > 0 && height > 0) {
        _size.width  = static_cast<uint32_t>(width);
        _size.height = static_cast<uint32_t>(height);
    }
    // 尺寸未变则忽略（应用启动时 onWindowSizeChanged 传来的尺寸与创建时一致，
    // 若此时重建 swapchain，会销毁管线引用的 renderPass 导致绘制崩溃）
    if (width > 0 && height > 0 &&
        (static_cast<uint32_t>(width)  != _swapchain.extent().width ||
         static_cast<uint32_t>(height) != _swapchain.extent().height)) {
        _swapchain.requestRecreate();
    }
}

void VulkanDevice::waitIdle() {
    if (_device != VK_NULL_HANDLE) vkDeviceWaitIdle(_device);
}

// ============================================================
// 资源创建
// ============================================================


// ---- v2 对象级销毁（B0）----
// V3-0 DeletionQueue：摘句柄立即生效（槽位清空、id 不复用）；真销毁登记到
// _frameCount + kLagFrames，beginFrame 的 fence 等待之后批量执行——资源不再
// 被帧在飞的命令引用。析构路径 flushAll 兜底。




// ---- B4：离屏渲染目标 ----




// ---- B4：同步回读（契约 ReadbackDesc；submitOneTime 等待完成）----

// ---- B4：RT 布局屏障（命令列表在 beginRenderPass/endRenderPass 的 RT 分支调用）----



// ============================================================
// 查询 / 其他
// ============================================================
VkImageView VulkanDevice::currentColorView() const {
    return _swapchain.colorView(_imageIndex);
}

// B1：beginRenderPass 前的附件布局屏障。
// color：PRESENT_SRC（或首帧 UNDEFINED）→ COLOR_ATTACHMENT_OPTIMAL；
// depth：UNDEFINED（首帧，loadOp CLEAR 丢弃旧内容）→ DEPTH_ATTACHMENT_OPTIMAL，
// 之后保持不变，每帧补一条同布局的访问链屏障（前帧写 → 本帧读写）。
void VulkanDevice::recordBeginPassBarriers(VkCommandBuffer cb, bool clearColor, bool clearDepth) {
    _swapchain.recordBeginPassBarriers(cb, _imageIndex, clearColor, clearDepth);
}


// B1：present 前把当前 image 转回 PRESENT_SRC（仅当本帧渲染过）。
// V3-0：本帧未渲染（acquire 后跳过，layout 仍 UNDEFINED，如 CHURN 压测的
// 帧外 beginFrame/endFrame 循环）也必须转换——present 要求 PRESENT_SRC。
void VulkanDevice::recordPresentBarrier(VkCommandBuffer cb) {
    _swapchain.recordPresentBarrier(cb, _imageIndex);
}

bool VulkanDevice::nativeHandles(NativeHandles& out) const {
    out.instance       = reinterpret_cast<uint64_t>(_instance);
    out.physicalDevice = reinterpret_cast<uint64_t>(_physicalDevice);
    out.device         = reinterpret_cast<uint64_t>(_device);
    out.queue          = reinterpret_cast<uint64_t>(_queue);
    out.renderPass     = 0;   // B1：dynamic rendering，无 renderpass（ImGui 走 UseDynamicRendering）
    out.queueFamily    = _queueFamily;
    out.minImageCount  = 2;
    out.imageCount     = _swapchain.imageCount();
    out.colorFormat    = static_cast<uint32_t>(_swapchain.format());
    out.depthFormat    = static_cast<uint32_t>(_swapchain.depthFormat());
    out.msaaSamples    = _swapchain.msaaSamples();
    return true;
}

IRHICommandList* VulkanDevice::commandList() {
    return _commandList.get();
}

VkCommandBuffer VulkanDevice::commandBuffer() const {
    return _commandList ? _commandList->commandBuffer() : VK_NULL_HANDLE;
}

// V3-6：附加命令列表。一帧内先于主列表提交，语义与"按顺序录在同一列表上"一致。
// 列表对象由设备持有（一次创建、逐帧复用），不适合每帧新建。
// V3-6 自检：验证"附加列表先提交、主列表后提交"的顺序。
// 附加列表每帧写 0xAAAA，主列表每帧末尾写 0xBBBB；回读应为 0xBBBB。
void VulkanDevice::multListSelfTest() {
    static const bool enabled = (std::getenv("RHI_VK_MULTILIST") != nullptr);
    if (!enabled) return;

    if (!_mlTest) {
        _mlTest = true;
        const char* v = std::getenv("RHI_VK_MULTILIST");
        _mlMode = (v && std::atoi(v) >= 2) ? 2 : 1;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size        = sizeof(uint32_t);
        bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(_device, &bci, nullptr, &_mlBuf));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(_device, _mlBuf, &req);
        _mlMem = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                      | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkBindBufferMemory(_device, _mlBuf, _mlMem.memory, _mlMem.offset));
        _mlMapped = _mlMem.mapped;
        _mlList = static_cast<VkCommandList*>(createCommandList());
        // 新列表加入 _extraLists 时本帧的 beginFrameSlot 循环已经跑过，这里补一次
        if (_mlList) _mlList->beginFrameSlot(_frames.index());
        ENGINE_LOG_INFO("[VK][multilist] self-test enabled (mode=%d: %s)",
                        _mlMode, _mlMode == 2 ? "worker thread" : "main thread");
    }

    if (_mlList) {
        if (_mlMode == 2) {
            // 真并发：工作线程往附加列表里录命令，主线程同时录场景。
            // 两条列表各持自己的 uniform 环与命令缓冲，只有共享设备的只读表。
            _mlWorker = std::thread([this] {
                vkCmdFillBuffer(_mlList->commandBuffer(), _mlBuf, 0,
                                sizeof(uint32_t), 0xAAAAAAAAu);
                _mlList->markUsed();
            });
        } else {
            vkCmdFillBuffer(_mlList->commandBuffer(), _mlBuf, 0, sizeof(uint32_t), 0xAAAAAAAAu);
            _mlList->markUsed();
        }
    }

    // 读第 20 帧的值：此时前几帧的提交早已完成（帧在飞 2 + 延迟销毁滞后）
    if (++_mlFrames == 20 && _mlMapped) {
        const uint32_t v = *static_cast<const uint32_t*>(_mlMapped);
        ENGINE_LOG_INFO("[VK][multilist] buffer=0x%08X (%s)", v,
                        v == 0xBBBBBBBBu ? "order ok" : "ORDER WRONG");
    }
}

IRHICommandList* VulkanDevice::createCommandList() {
    auto list = std::make_unique<VkCommandList>(this, false);
    if (!list->initCommandBuffers(_frames.slotCount())) {
        ENGINE_LOG_WARN_ONCE("[VK] secondary command list init failed; fallback to primary");
        return commandList();
    }
    VkCommandList* raw = list.get();
    _extraLists.push_back(std::move(list));
    // 若本帧已 begin（beginFrame 已过才创建），补一次 beginFrameSlot，
    // 否则本帧录制会拿到"未 begin"的命令缓冲（VUID-vkCmd*-commandBuffer-recording）
    if (_frameActive) raw->beginFrameSlot(_frames.index());
    return raw;
}

// 多线程录制：secondary command buffer（继承主 pass，被主列表 vkCmdExecuteCommands 执行）
IRHICommandList* VulkanDevice::createSecondaryCommandList() {
    auto list = std::make_unique<VkCommandList>(this, /*primary=*/false, /*secondary=*/true);
    if (!list->initCommandBuffers(_frames.slotCount())) {
        ENGINE_LOG_WARN_ONCE("[VK] secondary command list init failed; fallback to primary");
        return commandList();
    }
    VkCommandList* raw = list.get();
    _secondaryLists.push_back(std::move(list));
    return raw;
}

std::vector<VkCommandBuffer> VulkanDevice::usedSecondaryBuffers() const {
    std::vector<VkCommandBuffer> out;
    for (auto& l : _secondaryLists) {
        if (l->used()) out.push_back(l->commandBuffer());
    }
    return out;
}

// 附加列表销毁：从列表集合摘除，对象（含命令缓冲）等 GPU 用完再释放。
// 放在延迟队列里是为了覆盖"销毁发生在录制中途/本帧尚未提交"的情况。
void VulkanDevice::destroyCommandList(IRHICommandList* list) {
    if (!list) return;
    if (_commandList && list == _commandList.get()) {
        ENGINE_LOG_WARN_ONCE("[VK] destroyCommandList: primary list is device-owned, ignored");
        return;
    }
    for (auto it = _extraLists.begin(); it != _extraLists.end(); ++it) {
        if (it->get() != list) continue;
        VkCommandList* raw = it->release();
        _extraLists.erase(it);
        _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                       [raw] { delete raw; });
        return;
    }
    ENGINE_LOG_WARN_ONCE("[VK] destroyCommandList: unknown list (ignored)");
}

// RHI_TEX_CHURN=<N>：验证纹理槽位复用——循环创建/销毁 N 次，槽位数不增长。
// 用不带初始数据的纹理：不触发延迟上传，销毁是安全的；槽位复用本身与后端无关。
void VulkanDevice::textureSlotChurnTest() {
    static const char* env = std::getenv("RHI_TEX_CHURN");
    static uint32_t left = env ? static_cast<uint32_t>(std::atoi(env)) : 0;
    static size_t before = 0;
    static bool started = false;
    if (left == 0) return;
    if (!started) {
        if (_frameCount < 5) return;   // 等场景把自带纹理建完再取基线
        // 预热一次：让空闲链表里先有一个可复用槽，基线才稳定
        TextureDesc wd;
        wd.width = 1; wd.height = 1; wd.usage = TEX_SAMPLED;
        TextureHandle warm = createTexture(wd);
        if (warm.valid()) destroyTexture(warm);
        started = true;
        before = _textures.size();
    }

    // 每帧 100 个：销毁走延迟队列，必须跨帧 flush，否则一帧内堆到设备
    // 采样器上限（maxSamplerAllocationCount，常见 4000）之前就报错。
    const uint32_t batch = left < 100 ? left : 100;
    for (uint32_t i = 0; i < batch; ++i) {
        TextureDesc d;
        d.width  = 1;
        d.height = 1;
        d.usage  = TEX_SAMPLED;   // 无 initialData：不触发上传
        TextureHandle h = createTexture(d);
        if (!h.valid()) break;
        destroyTexture(h);
    }
    left -= batch;
    if (left == 0) {
        ENGINE_LOG_INFO("[VK][churn] texture slots: before=%zu after=%zu (slots stable=%s)",
                        before, _textures.size(),
                        _textures.size() == before ? "yes" : "NO");
    }
}

// RHI_CMDLIST_CHURN=<N>：验证"创建→录制→销毁"不会累积。
// 第一帧跑 N 轮，之后报告附加列表数量（应为 0），退出时看分配器 live。
void VulkanDevice::commandListChurnTest() {
    static const char* env = std::getenv("RHI_CMDLIST_CHURN");
    static const uint32_t rounds = env ? static_cast<uint32_t>(std::atoi(env)) : 0;
    if (rounds == 0 || _churnDone) return;
    _churnDone = true;

    // 压测用的落点缓冲（避免 fill 到未初始化的句柄）
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size        = sizeof(uint32_t);
    bci.usage       = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer scratch = VK_NULL_HANDLE;
    VkMemAlloc mem;
    if (vkCreateBuffer(_device, &bci, nullptr, &scratch) != VK_SUCCESS) return;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(_device, scratch, &req);
    mem = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                              | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkBindBufferMemory(_device, scratch, mem.memory, mem.offset));

    for (uint32_t i = 0; i < rounds; ++i) {
        IRHICommandList* l = createCommandList();
        if (!l || l == commandList()) break;   // 后退路径：拿不到独立列表
        auto* vkl = static_cast<VkCommandList*>(l);
        vkl->beginFrameSlot(_frames.index());
        vkCmdFillBuffer(vkl->commandBuffer(), scratch, 0, sizeof(uint32_t), i);
        destroyCommandList(l);
    }
    ENGINE_LOG_INFO("[VK][churn] command list rounds=%u, live extra lists=%zu",
                    rounds, _extraLists.size());

    // 落点缓冲本身不急着销毁：走延迟队列，避免和压测里未提交的命令抢生命周期
    _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames, [this, scratch, mem] {
        vkDestroyBuffer(_device, scratch, nullptr);
        _memAlloc.free(mem);
    });
}

// V3-3：管线创建/缓存交给 VkPipelines；设备只保留句柄表
PipelineHandle VulkanDevice::createPipeline(const GraphicsPipelineDesc& desc) {
    if (_deviceLost) return {};
    VkPipelineRes res;
    if (!_pipes.create(desc, res)) {
        return {};
    }
    RhiHandle base = _pipelines.alloc(std::move(res));
    PipelineHandle h; h.id = base.id; h.gen = base.gen;
    return h;
}

// V3-3：描述符/绑定资源统一交给 VkDescriptors（布局、池、帧 uniform 环）





uint64_t VulkanDevice::nativeBuffer(BufferHandle h) const {
    return _buffers.alive(h) ? reinterpret_cast<uint64_t>(_buffers.get(h.id).buffer) : 0;
}
uint64_t VulkanDevice::nativeTexture(TextureHandle h) const {
    return _textures.alive(h) ? reinterpret_cast<uint64_t>(_textures.get(h.id).image) : 0;
}
uint64_t VulkanDevice::nativePipeline(PipelineHandle h) const {
    return _pipelines.alive(h) ? reinterpret_cast<uint64_t>(_pipelines.get(h.id).pipeline) : 0;
}

// V3-1：句柄有效性（id 范围 + gen 代际校验）
bool VulkanDevice::alive(BufferHandle h)   const { return _buffers.alive(h); }
bool VulkanDevice::alive(TextureHandle h)  const { return _textures.alive(h); }
bool VulkanDevice::alive(PipelineHandle h) const { return _pipelines.alive(h); }

DeviceInfo VulkanDevice::info() const {
    DeviceInfo di;
    di.backend     = Backend::Vulkan;
    di.backendName = "Vulkan (MoltenVK)";
    di.adapterName = _adapterName;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(_physicalDevice, &props);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Vulkan %u.%u.%u",
                  VK_VERSION_MAJOR(props.apiVersion),
                  VK_VERSION_MINOR(props.apiVersion),
                  VK_VERSION_PATCH(props.apiVersion));
    di.apiVersion = buf;
    return di;
}

DeviceCapabilities VulkanDevice::capabilities() const {
    DeviceCapabilities caps;
    caps.maxTexture2D           = _caps.props.limits.maxImageDimension2D;
    caps.maxPushConstantSize    = _caps.props.limits.maxPushConstantsSize;
    caps.maxUniformBufferRange  = _caps.props.limits.maxUniformBufferRange;
    caps.maxSampledImagesPerSet = _caps.props.limits.maxDescriptorSetSampledImages;
    for (uint32_t i = 0; i < _caps.memory.memoryHeapCount; ++i) {
        const auto& h = _caps.memory.memoryHeaps[i];
        if (h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) caps.deviceMemoryBytes += h.size;
    }
    caps.dynamicRendering   = _caps.hasDynamicRendering;
    caps.synchronization2   = _caps.hasSynchronization2;
    caps.timelineSemaphore  = _caps.hasTimelineSemaphore;
    caps.descriptorIndexing = _caps.hasDescriptorIndexing;
    caps.memoryBudget       = _caps.hasMemoryBudget;
    caps.wideLines          = _caps.features.wideLines == VK_TRUE;
    caps.geometryShader     = _caps.features.geometryShader == VK_TRUE;
    caps.samplerAnisotropy  = _caps.features.samplerAnisotropy == VK_TRUE;
    for (const auto& f : _caps.surfaceFormats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) {
            caps.sRGBSwapchain = true;
            break;
        }
    }

    caps.details = _caps.toCapabilities();
    caps.details.push_back({"验证层", _validationEnabled ? "enabled" : "disabled"});
    caps.details.push_back({"debug_utils", _debugUtilsEnabled ? "enabled" : "disabled"});
    if (_pipes.cacheActive()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s, 启动时 %zu B",
                      _pipes.cacheFromDisk() ? "loaded" : "new",
                      _pipes.cacheBytes());
        caps.details.push_back({"pipeline cache", buf});
    } else {
        caps.details.push_back({"pipeline cache", "disabled"});
    }
    return caps;
}

} // namespace rhi
