//
//  RHIFactory.cpp
//
#include "log/LogMacros.h"
#include "RHIFactory.h"
#include "gl/GLDevice.h"
#ifdef ENGINE_HAS_VULKAN
#include "vk/VkDevice.h"
#endif
#include <iostream>

namespace rhi {

std::unique_ptr<IRHIDevice> RHIFactory::createDevice(Backend backend,
                                                     GLFWwindow* window,
                                                     const std::string& shaderRootPath) {
    switch (backend) {
        case Backend::OpenGL:
            return std::make_unique<GLDevice>(window, shaderRootPath);
        case Backend::Vulkan:
#ifdef ENGINE_HAS_VULKAN
            return std::make_unique<VulkanDevice>(window, shaderRootPath);
#else
            ENGINE_LOG_WARN("[RHI] Vulkan backend not compiled (ENGINE_HAS_VULKAN undefined)");
            return nullptr;
#endif
    }
    return nullptr;
}

std::unique_ptr<IRHIDevice> RHIFactory::createDevice(const DeviceDesc& desc) {
    // B0 契约入口：宿主窗口/离屏设备（C1）落地前返回 nullptr 并明确告警，
    // 避免上层误以为拿到了可用设备。
    (void)desc;
    ENGINE_LOG_WARN("[RHI] DeviceDesc-based device creation not implemented yet (C1); "
                    "use RHIFactory::createDevice(Backend, GLFWwindow*, shaderRootPath)");
    return nullptr;
}

} // namespace rhi
