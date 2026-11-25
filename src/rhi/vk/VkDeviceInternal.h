//
//  VkDeviceInternal.h
//  后端内部共享小工具（多个 VkDevice*.cpp 用）
//
#ifndef VkDeviceInternal_h
#define VkDeviceInternal_h

#include "VkCommon.h"

namespace rhi {

// 选一个支持的深度格式（D32F 优先，退化到 D24S8）
static inline VkFormat findSupportedDepthFormat(VkPhysicalDevice pd) {
    const VkFormat candidates[] = {
        VK_FORMAT_D32_SFLOAT,
        VK_FORMAT_D32_SFLOAT_S8_UINT,
        VK_FORMAT_D24_UNORM_S8_UINT,
    };
    for (VkFormat f : candidates) {
        VkFormatProperties props;
        vkGetPhysicalDeviceFormatProperties(pd, f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            return f;
        }
    }
    return VK_FORMAT_D32_SFLOAT;   // 兜底（现代设备都支持 D32）
}

} // namespace rhi

#endif /* VkDeviceInternal_h */
