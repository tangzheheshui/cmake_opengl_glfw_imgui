//
//  VkDescriptors.h
//  RHI Vulkan 后端 —— 描述符布局与描述符池（V3-3 拆出，V3-14 收窄职责）
//
//  归属的东西：
//    · 两个描述符布局：set0 = 帧数据（动态偏移 UBO），set1..N = 纹理槽
//    · 全局管线布局（所有管线共用）
//    · 纹理描述符池（每张纹理一个 set，创建时写一次）
//    · 帧数据描述符池（每条命令列表 × 每个帧槽位一个 set）
//
//  V3-14：uniform 环搬进各命令列表（每列表每帧槽位一份）。这样两条列表在
//  不同线程录制时不会抢同一个环游标；这里只负责按给定 buffer 造 set0。
//
#ifndef VkDescriptors_h
#define VkDescriptors_h

#include "VkCommon.h"
#include <functional>
#include <vector>

namespace rhi {

class VkDescriptors {
public:
    // 对象命名回调（RenderDoc/capture 可读；为空时跳过）
    using NameFn = std::function<void(VkObjectType, uint64_t, const char*)>;

    // texSlotCount = 实际可用的采样槽数（<= kMaxTexSlots，由设备能力裁）
    bool init(VkDevice device, uint32_t minUboAlign, VkMemoryAllocator* alloc,
              uint32_t texSlotCount, NameFn nameFn);
    void shutdown();

    uint32_t texSlotCount() const { return _texSlotCount; }
    uint32_t minAlign() const { return _minAlign < 256 ? 256 : _minAlign; }
    VkPipelineLayout pipelineLayout() const { return _pipelineLayout; }

    // 为一条命令列表的某个帧槽位造 set0：指向它自己的 uniform 环。
    // tag 只用于调试对象命名。池耗尽会自动扩容。
    bool createFrameSet(VkBuffer ringBuffer, uint32_t tag, VkDescriptorSet& outSet);

    // 白纹理兜底：未绑定的槽位、上传还没完成的纹理
    VkDescriptorSet fallbackSet(uint32_t slot) const {
        // 槽 3 约定给 cubemap（天空盒）；其余槽用 2D 白纹理
        return (slot == 3) ? _whiteCubeSet : _whiteSet;
    }
    void setFallbackSets(VkDescriptorSet white, VkDescriptorSet whiteCube) {
        _whiteSet = white;
        _whiteCubeSet = whiteCube;
    }

    // ---- 资源创建期 ----
    // 分配一枚纹理槽描述符集并写入 sampler/view（写一次，之后只绑不更新）
    bool createTextureSet(VkSampler sampler, VkImageView view, VkDescriptorSet& outSet);

private:
    void createPoolsAndLayouts(uint32_t texSlotCount);
    void growTexturePool();
    void growFramePool();

    VkDevice           _device = VK_NULL_HANDLE;
    VkMemoryAllocator* _alloc  = nullptr;
    NameFn             _nameFn;

    VkDescriptorSetLayout _frameSetLayout   = VK_NULL_HANDLE;
    VkDescriptorSetLayout _texSlotSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout      _pipelineLayout   = VK_NULL_HANDLE;

    uint32_t _minAlign = 256;    // minUniformBufferOffsetAlignment（至少 256）
    uint32_t _texSlotCount = 4;  // 实际采样槽数（<= kMaxTexSlots）

    // 帧 set 池：数量 = 命令列表数 × 帧槽位数，用满追加
    std::vector<VkDescriptorPool> _framePools;
    uint32_t _frameCapSets = 16;

    std::vector<VkDescriptorPool> _texPools;
    uint32_t _poolCapSets    = 256;
    uint32_t _poolCapSampler = 1024;

    VkDescriptorSet _whiteSet     = VK_NULL_HANDLE;
    VkDescriptorSet _whiteCubeSet = VK_NULL_HANDLE;
};

} // namespace rhi

#endif /* VkDescriptors_h */
