//
//  VkDescriptors.h
//  RHI Vulkan 后端 —— 描述符与绑定资源（V3-3 从 VulkanDevice 拆出）
//
//  归属的东西：
//    · 两个描述符布局：set0 = 帧 uniform 环（动态偏移 UBO），set1..4 = 纹理槽
//    · 全局管线布局（所有管线共用）
//    · 纹理描述符池（每张纹理一个 set，创建时写一次）
//    · 帧 uniform 环（每帧归零，绘制数据顺序追加）
//
//  命令列表用到的入口：frameSet()/defaultUniformOffset()/allocDrawUniforms()/
//  fallbackSet()。纹理创建走 createTextureSet()。
//
#ifndef VkDescriptors_h
#define VkDescriptors_h

#include "VkCommon.h"
#include "../detail/FrameUniformRing.h"
#include <functional>
#include <vector>

namespace rhi {

class VkDescriptors {
public:
    // 对象命名回调（RenderDoc/capture 可读；为空时跳过）
    using NameFn = std::function<void(VkObjectType, uint64_t, const char*)>;

    bool init(VkDevice device, uint32_t minUboAlign, VkMemoryAllocator* alloc, NameFn nameFn);
    void shutdown();

    // ---- 每帧 ----
    // 环归零并预留 offset 0 的零数据块（没调 setDrawUniforms 的绘制用它）
    void beginFrame();
    // 追加一次绘制数据，返回动态偏移；环满会扩容后重试
    uint32_t allocDrawUniforms(const void* data, uint32_t size);
    uint32_t defaultUniformOffset() const { return _defaultUniformOffset; }

    // ---- 绘制期 ----
    VkDescriptorSet frameSet() const { return _frameSet; }
    // 白纹理兜底：未绑定的槽位、上传还没完成的纹理
    VkDescriptorSet fallbackSet(uint32_t slot) const {
        return (slot == kMaxTexSlots - 1) ? _whiteCubeSet : _whiteSet;
    }
    void setFallbackSets(VkDescriptorSet white, VkDescriptorSet whiteCube) {
        _whiteSet = white;
        _whiteCubeSet = whiteCube;
    }

    // ---- 资源创建期 ----
    // 分配一枚纹理槽描述符集并写入 sampler/view（写一次，之后只绑不更新）
    bool createTextureSet(VkSampler sampler, VkImageView view, VkDescriptorSet& outSet);
    VkPipelineLayout pipelineLayout() const { return _pipelineLayout; }

private:
    void createPoolsAndLayouts();
    void growTexturePool();
    void writeFrameSet();
    void growRing();

    VkDevice           _device = VK_NULL_HANDLE;
    VkMemoryAllocator* _alloc  = nullptr;
    NameFn             _nameFn;

    VkDescriptorSetLayout _frameSetLayout   = VK_NULL_HANDLE;
    VkDescriptorSetLayout _texSlotSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool      _frameDescPool    = VK_NULL_HANDLE;
    VkDescriptorSet       _frameSet         = VK_NULL_HANDLE;
    VkPipelineLayout      _pipelineLayout   = VK_NULL_HANDLE;

    uint32_t _minAlign = 256;   // minUniformBufferOffsetAlignment（至少 256）
    uint32_t _defaultUniformOffset = 0;
    detail::FrameUniformRing _ring;

    std::vector<VkDescriptorPool> _texPools;
    uint32_t _poolCapSets    = 256;
    uint32_t _poolCapSampler = 1024;

    VkDescriptorSet _whiteSet     = VK_NULL_HANDLE;
    VkDescriptorSet _whiteCubeSet = VK_NULL_HANDLE;
};

} // namespace rhi

#endif /* VkDescriptors_h */
