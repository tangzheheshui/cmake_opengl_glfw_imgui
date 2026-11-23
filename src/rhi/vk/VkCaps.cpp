//
//  VkCaps.cpp
//  RHI Vulkan 后端 - 物理设备能力探测（A3）
//
#include "VkCaps.h"
#include <cstdio>
#include <cstring>

namespace rhi {

static const char* deviceTypeStr(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "cpu";
        default:                                     return "other";
    }
}

static const char* presentModeStr(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_IMMEDIATE_KHR:    return "IMMEDIATE";
        case VK_PRESENT_MODE_MAILBOX_KHR:      return "MAILBOX";
        case VK_PRESENT_MODE_FIFO_KHR:         return "FIFO";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
        default:                               return "OTHER";
    }
}

static std::string formatStr(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_SRGB: return "B8G8R8A8_SRGB";
        case VK_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_SRGB: return "R8G8B8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10_UNORM";
        default: {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "VkFormat(%d)", static_cast<int>(f));
            return buf;
        }
    }
}

static std::string yesNo(bool v) { return v ? "yes" : "no"; }

VkCaps VkCaps::query(VkPhysicalDevice pd, VkSurfaceKHR surface) {
    VkCaps c;
    vkGetPhysicalDeviceProperties(pd, &c.props);
    vkGetPhysicalDeviceFeatures(pd, &c.features);
    vkGetPhysicalDeviceMemoryProperties(pd, &c.memory);

    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, nullptr);
    c.queueFamilies.resize(count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &count, c.queueFamilies.data());

    count = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> exts(count);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &count, exts.data());
    for (auto& e : exts) c.extensions.emplace_back(e.extensionName);

    c.hasDescriptorIndexing = c.hasExtension(VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME);
    c.hasDynamicRendering   = c.hasExtension(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
    c.hasSynchronization2   = c.hasExtension(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    c.hasTimelineSemaphore  = c.hasExtension(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
    c.hasMemoryBudget       = c.hasExtension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    c.hasPortabilitySubset  = c.hasExtension("VK_KHR_portability_subset");

    if (surface != VK_NULL_HANDLE) {
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, &c.surface);
        count = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &count, nullptr);
        c.presentModes.resize(count);
        vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &count, c.presentModes.data());

        count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, nullptr);
        c.surfaceFormats.resize(count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surface, &count, c.surfaceFormats.data());
    }
    return c;
}

bool VkCaps::hasExtension(const char* name) const {
    for (auto& e : extensions) {
        if (std::strcmp(e.c_str(), name) == 0) return true;
    }
    return false;
}

std::vector<DeviceCapability> VkCaps::toCapabilities() const {
    std::vector<DeviceCapability> out;
    auto add = [&out](const std::string& n, const std::string& v) {
        out.push_back({n, v});
    };
    char buf[256];

    add("设备", props.deviceName);
    add("设备类型", deviceTypeStr(props.deviceType));
    std::snprintf(buf, sizeof(buf), "%u.%u.%u",
                  VK_VERSION_MAJOR(props.apiVersion),
                  VK_VERSION_MINOR(props.apiVersion),
                  VK_VERSION_PATCH(props.apiVersion));
    add("API 版本", buf);
    std::snprintf(buf, sizeof(buf), "%u.%u.%u (vendor 0x%04x, device 0x%04x)",
                  VK_VERSION_MAJOR(props.driverVersion),
                  VK_VERSION_MINOR(props.driverVersion),
                  VK_VERSION_PATCH(props.driverVersion),
                  props.vendorID, props.deviceID);
    add("驱动版本", buf);

    // 队列族：每个族一行，标明 graphics/compute/transfer 与队列数
    for (size_t i = 0; i < queueFamilies.size(); ++i) {
        const auto& q = queueFamilies[i];
        std::string flags;
        if (q.queueFlags & VK_QUEUE_GRAPHICS_BIT) flags += "graphics ";
        if (q.queueFlags & VK_QUEUE_COMPUTE_BIT)  flags += "compute ";
        if (q.queueFlags & VK_QUEUE_TRANSFER_BIT) flags += "transfer ";
        if (flags.empty()) flags = "- ";
        std::snprintf(buf, sizeof(buf), "族 %zu: %u 队列 [%s]", i, q.queueCount, flags.c_str());
        add("队列", buf);
    }

    // 内存堆（E1/VMA 预估显存预算用）
    for (uint32_t i = 0; i < memory.memoryHeapCount; ++i) {
        const auto& h = memory.memoryHeaps[i];
        std::snprintf(buf, sizeof(buf), "heap%u: %.0f MB%s", i,
                      static_cast<double>(h.size) / (1024.0 * 1024.0),
                      (h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device-local)" : "");
        add("显存堆", buf);
    }

    std::snprintf(buf, sizeof(buf), "%u B", props.limits.maxPushConstantsSize);
    add("push constants 上限", buf);
    std::snprintf(buf, sizeof(buf), "%u", props.limits.maxImageDimension2D);
    add("最大 2D 纹理", buf);
    std::snprintf(buf, sizeof(buf), "%u B", props.limits.maxUniformBufferRange);
    add("UBO 范围上限", buf);
    std::snprintf(buf, sizeof(buf), "%u", props.limits.maxDescriptorSetSampledImages);
    add("每 set 采样纹理上限", buf);

    if (!surfaceFormats.empty()) {
        std::string s;
        for (size_t i = 0; i < surfaceFormats.size() && i < 4; ++i) {
            if (i) s += ", ";
            s += formatStr(surfaceFormats[i].format);
        }
        if (surfaceFormats.size() > 4) s += ", ...";
        add("surface 格式", s);
    }
    if (!presentModes.empty()) {
        std::string s;
        for (size_t i = 0; i < presentModes.size(); ++i) {
            if (i) s += ", ";
            s += presentModeStr(presentModes[i]);
        }
        add("present 模式", s);
    }
    std::snprintf(buf, sizeof(buf), "%u..%u", surface.minImageCount, surface.maxImageCount);
    add("swapchain 图像数", buf);

    // 关键特性（CAD 显示与后续阶段相关）
    add("samplerAnisotropy", yesNo(features.samplerAnisotropy == VK_TRUE));
    add("geometryShader", yesNo(features.geometryShader == VK_TRUE));
    add("tessellationShader", yesNo(features.tessellationShader == VK_TRUE));
    add("fillModeNonSolid", yesNo(features.fillModeNonSolid == VK_TRUE));
    add("wideLines", yesNo(features.wideLines == VK_TRUE));
    add("depthClamp", yesNo(features.depthClamp == VK_TRUE));

    // 路线图相关扩展（B1/E1/E2 的前置条件）
    add("VK_EXT_descriptor_indexing", yesNo(hasDescriptorIndexing));
    add("VK_KHR_dynamic_rendering", yesNo(hasDynamicRendering));
    add("VK_KHR_synchronization2", yesNo(hasSynchronization2));
    add("VK_KHR_timeline_semaphore", yesNo(hasTimelineSemaphore));
    add("VK_EXT_memory_budget", yesNo(hasMemoryBudget));
    add("VK_KHR_portability_subset", yesNo(hasPortabilitySubset));

    return out;
}

void VkCaps::log() const {
    ENGINE_LOG_INFO("[VK] ---- device capabilities ----");
    for (auto& cap : toCapabilities()) {
        ENGINE_LOG_INFO("[VK]   %s: %s", cap.name.c_str(), cap.value.c_str());
    }
    ENGINE_LOG_INFO("[VK] ---- end capabilities ----");
}

} // namespace rhi
