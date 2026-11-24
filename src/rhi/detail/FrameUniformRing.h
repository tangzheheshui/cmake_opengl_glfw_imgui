//
//  FrameUniformRing.h
//  RHI Vulkan 后端 —— 帧内 uniform 环形缓冲（V3-2 绑定层）
//
//  背景：V3-2 之前每个绘制对象（每个 mesh）各持有一份 UBO buffer + 描述符集，
//  绘制时还要 vkUpdateDescriptorSets 去写纹理槽。对象一多，显存、描述符、
//  CPU 开销都随 mesh 数线性上涨；已录制的绘制还会被后续描述符写入污染，
//  只能靠"每个对象一个块"绕开。
//
//  V3-2 改为：
//    · 一块 1MB 的 HOST_VISIBLE 环形缓冲，每帧开头归零，绘制级 uniform
//      顺序追加（按设备对齐要求切块），不再按对象分配；
//    · 描述符集只描述"读这块环的 N 字节"，具体位置由动态偏移给出，
//      所以整帧共用一个描述符集，draw 时只多传一个 offset。
//
//  线程约定：主线程录制（与设备其余接口一致），不加锁。
//
#ifndef FrameUniformRing_h
#define FrameUniformRing_h

#include "../vk/VkMemoryAllocator.h"
#include <cstdint>

namespace rhi {
namespace detail {

class FrameUniformRing {
public:
    bool init(VkDevice device, VkMemoryAllocator* allocator,
              uint32_t bytes, uint32_t alignment);
    void shutdown();

    // 每帧开头归零（device->beginFrame 在 fence 等待之后调用）
    void reset() { _cursor = 0; }

    // 顺序追加：返回对齐后的字节偏移；空间不足返回 UINT32_MAX
    //（调用方扩容后重试，见 VulkanDevice::allocDrawUniforms）
    uint32_t alloc(const void* data, uint32_t size);

    // 扩容（翻倍策略）。旧缓冲立即回收：环只在录制期写，上一帧命令已由
    // beginFrame 的 fence 等待完成，不存在在飞引用。
    bool grow(uint32_t minBytes);

    VkBuffer buffer() const   { return _buffer; }
    uint32_t capacity() const { return _capacity; }
    uint32_t cursor() const   { return _cursor; }
    bool     valid() const    { return _buffer != VK_NULL_HANDLE; }

private:
    bool createBuffer(uint32_t bytes);
    void destroyBuffer();

    VkDevice           _device = VK_NULL_HANDLE;
    VkMemoryAllocator* _alloc  = nullptr;
    VkBuffer           _buffer = VK_NULL_HANDLE;
    VkMemAlloc         _mem;
    void*              _mapped   = nullptr;
    uint32_t           _capacity = 0;
    uint32_t           _cursor   = 0;
    uint32_t           _align    = 256;
};

} // namespace detail
} // namespace rhi

#endif /* FrameUniformRing_h */
