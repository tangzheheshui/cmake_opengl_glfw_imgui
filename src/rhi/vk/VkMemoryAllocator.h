//
//  VkMemoryAllocator.h —— E1: VkDeviceMemory 子分配器（最小可用版）
//
//  背景：此前每资源一次 vkAllocateMemory，maxMemoryAllocationCount（典型 4096）
//  在 CAD 上万 buffer 场景必然爆掉。本分配器做两级：
//    · 小分配（< 1MB）→ 8MB 页内 first-fit 空闲链表子分配，同内存类型共享页；
//      HOST_VISIBLE 页持久映射，子分配直接拿映射指针（省 per-object map/unmap）
//    · 大分配（≥ 1MB）→ 专用 vkAllocateMemory（纹理等大资源，数量级可控）
//  free 归还页内块并做邻接合并；页引用计数归零且整页空闲时回收页。
//
//  线程约定：全部主线程调用（与设备其余接口一致），不加锁。
//  不做：去碎片（Defragmentation）、预算查询、跨队列——随 E2 需要再加。
//

#ifndef VkMemoryAllocator_h
#define VkMemoryAllocator_h

#include <vulkan/vulkan.h>
#include <vector>

namespace rhi {

struct VkMemAlloc {
    VkDeviceMemory memory   = VK_NULL_HANDLE;
    VkDeviceSize   offset   = 0;
    void*          mapped   = nullptr;        // HOST_VISIBLE 时非空（页/专用持久映射，已含 offset）
    // 归属页身份 = 页 memory 句柄（不可用下标：页回收会 erase 使后续下标错位）；
    // VK_NULL_HANDLE = 专用分配
    VkDeviceMemory pageMem  = VK_NULL_HANDLE;
    VkDeviceSize   size     = 0;
    bool           valid() const { return memory != VK_NULL_HANDLE; }
};

class VkMemoryAllocator {
public:
    void init(VkPhysicalDevice pd, VkDevice device);
    void shutdown();   // 释放全部页/专用分配；仍有存活分配时告警（泄漏）

    /// 按 requirements 分配（调用方先 vkGet*MemoryRequirements）
    VkMemAlloc alloc(const VkMemoryRequirements& req, VkMemoryPropertyFlags props);

    void free(const VkMemAlloc& a);

    // 统计（调试面板/日志）
    size_t liveAllocations() const { return _liveCount; }
    size_t pageCount() const { return _pages.size(); }
    size_t dedicatedCount() const { return _dedicated.size(); }

private:
    static constexpr VkDeviceSize kPageSize           = 8ull * 1024 * 1024;   // 8MB
    static constexpr VkDeviceSize kDedicatedThreshold = 1ull * 1024 * 1024;   // ≥1MB 走专用

    struct FreeBlock {
        VkDeviceSize offset = 0;
        VkDeviceSize size  = 0;
    };
    struct Page {
        uint32_t          memTypeIndex = 0xFFFFFFFFu;
        VkDeviceMemory    memory = VK_NULL_HANDLE;
        VkDeviceSize      size   = 0;
        void*             mapped = nullptr;   // HOST_VISIBLE 页持久映射
        std::vector<FreeBlock> freeList;       // 按 offset 升序
        size_t            refCount = 0;        // 活跃子分配数
    };
    struct Dedicated {
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void*          mapped = nullptr;
        VkDeviceSize   size   = 0;
    };

    VkMemAlloc allocDedicated(uint32_t memTypeIndex, const VkMemoryRequirements& req,
                              bool hostVisible);
    VkMemAlloc allocFromPage(uint32_t memTypeIndex, const VkMemoryRequirements& req,
                             bool hostVisible);
    uint32_t resolveMemoryType(uint32_t typeBits, VkMemoryPropertyFlags props) const;

    VkPhysicalDevice _pd     = VK_NULL_HANDLE;
    VkDevice         _device = VK_NULL_HANDLE;
    std::vector<Page>      _pages;
    std::vector<Dedicated> _dedicated;
    size_t                 _liveCount = 0;
};

} // namespace rhi

#endif // VkMemoryAllocator_h
