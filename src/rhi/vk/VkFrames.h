//
//  VkFrames.h
//  RHI Vulkan 后端 —— 帧级同步对象（V3-4 从 VulkanDevice 拆出）
//
//  一帧一份、轮转使用的东西：
//    · in-flight fence（提交完成信号）
//    · image-available 信号量（取到交换链图像）
//
//  命令缓冲不在这里：V3-6 起每条命令列表自己按槽位持有一组（见 VkCommandList）。
//
//  帧在飞数量 = 槽位数（默认 2）：第 N 帧等待第 N-2 帧的 fence，
//  所以 GPU 干活时 CPU 已经在录下一帧，不必每帧互等。
//
//  约定：
//    · 每个槽位的对象只由自己的那一帧使用，跨帧不共享；
//    · 交换链的 render-done 信号量仍按图像下标走（见 VkSwapchain）。
//
#ifndef VkFrames_h
#define VkFrames_h

#include "VkCommon.h"
#include <functional>
#include <vector>

namespace rhi {

class VkFrames {
public:
    using NameFn = std::function<void(VkObjectType, uint64_t, const char*)>;

    struct Slot {
        VkFence     fence          = VK_NULL_HANDLE;
        VkSemaphore imageAvailable = VK_NULL_HANDLE;
    };

    static constexpr uint32_t kDefaultSlots = 2;

    bool init(VkDevice device, uint32_t slots, NameFn nameFn);
    void shutdown();   // 命令缓冲随命令池回收，这里只销毁 fence/信号量

    uint32_t slotCount() const { return static_cast<uint32_t>(_slots.size()); }
    uint32_t index() const     { return _index; }
    Slot&    current()         { return _slots[_index]; }
    const Slot& current() const { return _slots[_index]; }

    // 等本槽位上一次提交完成（首帧 fence 预置为已信号，直接通过）
    bool wait();
    // 取到图像后清 fence（命令缓冲归各命令列表自己管理，见 VkCommandList）
    bool resetFence();
    // 提交完成后推进到下一槽位
    void advance() { _index = (_index + 1) % static_cast<uint32_t>(_slots.size()); }

private:
    VkDevice _device = VK_NULL_HANDLE;
    std::vector<Slot> _slots;
    uint32_t _index = 0;
};

} // namespace rhi

#endif /* VkFrames_h */
