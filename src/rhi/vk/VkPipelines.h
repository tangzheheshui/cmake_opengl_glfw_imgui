//
//  VkPipelines.h
//  RHI Vulkan 后端 —— 图形管线创建与管线缓存（V3-3 从 VulkanDevice 拆出）
//
//  归属的东西：
//    · SPIR-V 加载 + push constant 反射表（setUniform 按名定位）
//    · VkGraphicsPipelineCreateInfo 的全部状态烘焙
//    · 管线缓存（磁盘加载/保存，驱动或设备不匹配时丢弃）
//
//  设备只保留管线句柄表；这里只负责"造出一个管线 + 它的反射表"。
//
#ifndef VkPipelines_h
#define VkPipelines_h

#include "VkCommon.h"
#include "VkCaps.h"
#include <functional>
#include <string>

namespace rhi {

class VkPipelines {
public:
    using NameFn = std::function<void(VkObjectType, uint64_t, const char*)>;

    // 初始化：加载磁盘管线缓存（失败不致命，只是编译慢一点）
    void init(VkDevice device, const VkCaps* caps, std::string shaderRoot,
              VkPipelineLayout layout, NameFn nameFn);

    // 创建图形管线：成功时 out.pipeline 有效、out.uniformTable 填好
    bool create(const GraphicsPipelineDesc& desc, VkFormat colorFmt, VkFormat depthFmt,
                VkPipelineRes& out);

    // 退出时调用：导出缓存到磁盘，再销毁缓存对象
    void saveAndDestroy();

    // 状态查询（设备能力表/日志用）
    bool   cacheActive() const   { return _cache != VK_NULL_HANDLE; }
    bool   cacheFromDisk() const { return _cacheFromDisk; }
    size_t cacheBytes() const    { return _cacheBytes; }

private:
    void loadCache();
    void saveCache();

    VkDevice          _device = VK_NULL_HANDLE;
    const VkCaps*     _caps   = nullptr;
    std::string       _shaderRoot;
    VkPipelineLayout  _layout = VK_NULL_HANDLE;
    NameFn            _nameFn;
    VkPipelineCache   _cache  = VK_NULL_HANDLE;
    std::string       _cachePath;
    bool              _cacheFromDisk = false;
    size_t            _cacheBytes    = 0;
    uint32_t          _nextId        = 0;   // 仅用于调试对象命名
};

} // namespace rhi

#endif /* VkPipelines_h */
