//
//  RHIFactory.cpp
//
#include "log/LogMacros.h"
#include "RHIFactory.h"
#include "gl/GLDevice.h"
#ifdef ENGINE_HAS_VULKAN
#include "vk/VkDevice.h"
#endif
#include <memory>

namespace rhi {

std::unique_ptr<IRHIDevice> RHIFactory::createDevice(const DeviceDesc& desc,
                                                     std::string* outError) {
    auto fail = [outError](const char* why) {
        if (outError) *outError = why;
        ENGINE_LOG_ERROR("[RHI] createDevice failed: %s", why);
        return std::unique_ptr<IRHIDevice>();
    };

    if (desc.offscreen && !desc.window.valid()) {
        // 离屏设备：M3~B4 各后端已具备 RT 能力；swapchain-offscreen 创建在
        // C4/F1（headless）阶段补齐，此处显式拒绝避免半可用设备
        return fail("offscreen device creation not supported yet (C4/F1)");
    }
    if (!desc.window.valid()) {
        return fail("DeviceDesc.window invalid (host window required for on-screen device)");
    }

    switch (desc.backend) {
        case Backend::OpenGL: {
            auto dev = std::make_unique<GLDevice>(desc);
            if (!dev->initialized()) {
                return fail("GLDevice init failed (GL context not current? shaderRootPath missing?)");
            }
            return dev;
        }
        case Backend::Vulkan: {
#ifdef ENGINE_HAS_VULKAN
            std::string err;
            auto dev = std::make_unique<VulkanDevice>(desc, err);
            if (!err.empty()) {
                return fail(err.c_str());
            }
            if (!dev->initialized()) {
                return fail("VulkanDevice init failed");
            }
            return dev;
#else
            return fail("Vulkan backend not compiled (ENGINE_HAS_VULKAN undefined)");
#endif
        }
    }
    return fail("unknown backend");
}

} // namespace rhi
