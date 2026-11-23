//
//  VkCaps.h
//  RHI Vulkan 后端 - 物理设备能力探测（A3）
//
//  只在初始化阶段查询一次，缓存结果：
//  - 便于启动日志打印能力表（多机验证的第一手资料）
//  - 后续 B1（Dynamic Rendering）/ E1（VMA）/ E2（Timeline Semaphore）
//    按能力决定路径，而不是"猜某扩展存在"
//
#ifndef VkCaps_h
#define VkCaps_h

#include "VkCommon.h"
#include "../RhiTypes.h"
#include <string>
#include <vector>

namespace rhi {

struct VkCaps {
    VkPhysicalDeviceProperties       props{};
    VkPhysicalDeviceFeatures         features{};
    VkPhysicalDeviceMemoryProperties memory{};
    VkSurfaceCapabilitiesKHR         surface{};
    std::vector<VkPresentModeKHR>    presentModes;
    std::vector<VkSurfaceFormatKHR>  surfaceFormats;
    std::vector<VkQueueFamilyProperties> queueFamilies;
    std::vector<std::string>         extensions;   // 设备扩展名（便于 grep/日志）

    // 与路线图直接相关的扩展是否可用（不启用，仅探测）
    bool hasDescriptorIndexing = false;
    bool hasDynamicRendering   = false;
    bool hasSynchronization2   = false;
    bool hasTimelineSemaphore  = false;
    bool hasMemoryBudget       = false;
    bool hasPortabilitySubset  = false;

    static VkCaps query(VkPhysicalDevice pd, VkSurfaceKHR surface);

    // 设备扩展是否可用（按名字精确匹配）
    bool hasExtension(const char* name) const;

    // 能力表（供 IRHIDevice::capabilities() 与启动日志使用）
    std::vector<DeviceCapability> toCapabilities() const;
    void log() const;   // ENGINE_LOG_INFO 逐行打印
};

} // namespace rhi

#endif /* VkCaps_h */
