//
//  VulkanDevice.cpp
//
#include "log/LogMacros.h"
#include "VkDevice.h"
#include "VkCommandList.h"
#include <GLFW/glfw3.h>
#include <fstream>
#include <cstring>
#include <algorithm>
#include <filesystem>

namespace rhi {

static const char* kRequiredDeviceExts[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// ---- 工具：读取 SPIR-V 文件 ----
static std::vector<char> readSpirvFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open()) {
        ENGINE_LOG_ERROR("[VK] cannot open SPIR-V: %s", path.c_str());
        return {};
    }
    size_t size = static_cast<size_t>(f.tellg());
    std::vector<char> data(size);
    f.seekg(0);
    f.read(data.data(), size);
    return data;
}

// 深度附件格式：按支持度依次尝试
static VkFormat findSupportedDepthFormat(VkPhysicalDevice pd) {
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

static uint32_t findMemoryType(VkPhysicalDevice pd, uint32_t typeBits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(pd, &mem);
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) && (mem.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return UINT32_MAX;
}

// ---- B3：RHI Format → VkFormat（契约枚举冻结，见 RhiTypes.h）----
static VkFormat vkFormatOf(rhi::Format f) {
    switch (f) {
        case rhi::Format::RGBA8_UNorm: return VK_FORMAT_R8G8B8A8_UNORM;
        case rhi::Format::RGBA8_SRGB:  return VK_FORMAT_R8G8B8A8_SRGB;
        case rhi::Format::BGRA8_UNorm: return VK_FORMAT_B8G8R8A8_UNORM;
        case rhi::Format::BGRA8_SRGB:  return VK_FORMAT_B8G8R8A8_SRGB;
        case rhi::Format::R8_UNorm:    return VK_FORMAT_R8_UNORM;
        case rhi::Format::R16F:        return VK_FORMAT_R16_SFLOAT;
        case rhi::Format::RGBA16F:     return VK_FORMAT_R16G16B16A16_SFLOAT;
        case rhi::Format::RGBA32F:     return VK_FORMAT_R32G32B32A32_SFLOAT;
        case rhi::Format::Depth32F:    return VK_FORMAT_D32_SFLOAT;
        case rhi::Format::D24S8:       return VK_FORMAT_D24_UNORM_S8_UINT;
        case rhi::Format::R32_UInt:    return VK_FORMAT_R32_UINT;
    }
    return VK_FORMAT_R8G8B8A8_UNORM;
}

// 完整 mip 链级数（TextureDesc::mipLevels == 0 的语义）
static uint32_t fullMipChainLevels(uint32_t w, uint32_t h) {
    uint32_t m = std::max(w, h);
    uint32_t levels = 1;
    while (m > 1) { m >>= 1; ++levels; }
    return levels;
}

// blit 能力（mipmap 生成依赖；可选特性，运行时探测，不支持则回退单级）
static bool formatBlitSupported(VkPhysicalDevice pd, VkFormat f) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(pd, f, &p);
    return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0
        && (p.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
}

// ============================================================
// 创建 / 销毁
// ============================================================
VulkanDevice::VulkanDevice(GLFWwindow* window, const std::string& shaderRootPath)
    : _window(window)
    , _shaderRootPath(shaderRootPath) {
    createInstance();
    createSurface();
    pickPhysicalDevice();
    // A3: 能力探测（启动日志打印能力表，多机验证的第一手资料）
    _caps = VkCaps::query(_physicalDevice, _surface);
    _caps.log();
    createLogicalDevice();
    createPipelineCache();   // A3: 需在逻辑设备之后、创建任何管线之前
    createSwapchainObjects();
    createCommandObjects();
    createSyncObjects();
    // A1: 加载对象命名函数指针（debug_utils 扩展启用后可用）
    _setNameFn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(_instance, "vkSetDebugUtilsObjectNameEXT"));
    _commandList = std::make_unique<VkCommandList>(this);
    createWhiteFallbackTexture();   // 先于描述符对象：默认 uniform block 需引用白纹理
    createDescriptorObjects();
}

VulkanDevice::~VulkanDevice() {
    if (_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(_device);
    }
    destroySwapchainObjects();
    if (_fenceInFlight)     vkDestroyFence(_device, _fenceInFlight, nullptr);
    if (_semImageAvailable) vkDestroySemaphore(_device, _semImageAvailable, nullptr);
    // _semRenderDone 为 per-image，随 swapchain 销毁（destroySwapchainObjects）
    if (_commandPool)       vkDestroyCommandPool(_device, _commandPool, nullptr);
    for (auto& b : _buffers) {
        if (b.buffer) vkDestroyBuffer(_device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(_device, b.memory, nullptr);
    }
    for (auto& t : _textures) {
        if (t.sampler) vkDestroySampler(_device, t.sampler, nullptr);
        if (t.view)    vkDestroyImageView(_device, t.view, nullptr);
        if (t.image)   vkDestroyImage(_device, t.image, nullptr);
        if (t.memory)  vkFreeMemory(_device, t.memory, nullptr);
    }
    for (auto& b : _uniformBlocks) {
        if (b.buffer) vkDestroyBuffer(_device, b.buffer, nullptr);
        if (b.memory) vkFreeMemory(_device, b.memory, nullptr);
        // 描述符集随 pool 销毁
    }
    // destroySwapchainObjects 只回收 swapchain/renderPass，不回收管线
    for (auto& p : _pipelines) {
        if (p.pipeline) vkDestroyPipeline(_device, p.pipeline, nullptr);
    }
    _pipelines.clear();
    if (_pipelineLayout)    vkDestroyPipelineLayout(_device, _pipelineLayout, nullptr);
    if (_texDescriptorPool) vkDestroyDescriptorPool(_device, _texDescriptorPool, nullptr);
    if (_texSetLayout)      vkDestroyDescriptorSetLayout(_device, _texSetLayout, nullptr);
    savePipelineCache();     // A3: 管线销毁后仍可导出缓存内容；在 destroy device 之前
    if (_pipelineCache)     vkDestroyPipelineCache(_device, _pipelineCache, nullptr);
    if (_device)   vkDestroyDevice(_device, nullptr);
    if (_surface)  vkDestroySurfaceKHR(_instance, _surface, nullptr);
    if (_debugMessenger && _destroyDebugMessengerFn) {
        _destroyDebugMessengerFn(_instance, _debugMessenger, nullptr);
    }
    if (_instance) vkDestroyInstance(_instance, nullptr);
}

void VulkanDevice::createInstance() {
    // 1) GLFW 要求的平台 surface 扩展（macOS: VK_KHR_surface + VK_EXT_metal_surface）
    std::vector<const char*> exts;
    {
        uint32_t n = 0;
        const char** e = glfwGetRequiredInstanceExtensions(&n);
        exts.assign(e, e + n);
        for (uint32_t i = 0; i < n; ++i) {
            ENGINE_LOG_ERROR("[VK] glfw ext[%u] = %s", i, e[i]);
        }
    }

    // 2) 校验并启用 portability 枚举（macOS 必须，MoltenVK 是 portability 驱动）
    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> avail(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, avail.data());
    bool hasPortability = false, hasDebugUtils = false, hasProps2 = false;
    for (auto& e : avail) {
        if (std::strcmp(e.extensionName, "VK_KHR_portability_enumeration") == 0) hasPortability = true;
        if (std::strcmp(e.extensionName, "VK_EXT_debug_utils") == 0) hasDebugUtils = true;
        if (std::strcmp(e.extensionName, "VK_KHR_get_physical_device_properties2") == 0) hasProps2 = true;
    }
    if (hasPortability) exts.push_back("VK_KHR_portability_enumeration");
    if (hasDebugUtils) exts.push_back("VK_EXT_debug_utils");   // A1: 对象命名 + 调试回调（独立于验证层）
    if (hasProps2) exts.push_back("VK_KHR_get_physical_device_properties2");   // portability_subset / descriptor_indexing 的依赖

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "my_app";
    app.apiVersion       = VK_API_VERSION_1_0;   // 最低版本请求，规避 loader/MVK 版本协商问题

    // A1: 验证层开关（RHI_VK_VALIDATION=1 强制开 / =0 强制关；Debug 构建默认开）
    const char* venv = std::getenv("RHI_VK_VALIDATION");
    bool wantValidation = false;
    if (venv) {
        wantValidation = (std::strcmp(venv, "0") != 0);
    }
#ifndef NDEBUG
    else {
        wantValidation = true;   // Debug 构建默认开启
    }
#endif
    _validationEnabled = false;
    std::vector<const char*> layers;
    if (wantValidation) {
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> avail(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, avail.data());
        for (const auto& l : avail) {
            if (std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                _validationEnabled = true;
                break;
            }
        }
        if (!_validationEnabled) {
            ENGINE_LOG_WARN("[VK] validation requested but VK_LAYER_KHRONOS_validation not installed");
        }
    }

    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount   = static_cast<uint32_t>(exts.size());
    ci.ppEnabledExtensionNames = exts.data();
    ci.enabledLayerCount       = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames     = layers.empty() ? nullptr : layers.data();
    if (hasPortability) {
        ci.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    VK_CHECK(vkCreateInstance(&ci, nullptr, &_instance));
    _debugUtilsEnabled = hasDebugUtils;

    if (_validationEnabled) {
        ENGINE_LOG_INFO("[VK] validation layer enabled");
        createDebugMessenger();
    }
}

void VulkanDevice::createDebugMessenger() {
    _createDebugMessengerFn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(_instance, "vkCreateDebugUtilsMessengerEXT"));
    _destroyDebugMessengerFn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
        vkGetInstanceProcAddr(_instance, "vkDestroyDebugUtilsMessengerEXT"));
    if (!_createDebugMessengerFn) return;

    VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
    mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT
                       | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
    mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                   | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                   | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    mi.pfnUserCallback = VulkanDevice::debugCallback;
    VK_CHECK(_createDebugMessengerFn(_instance, &mi, nullptr, &_debugMessenger));
}

VKAPI_ATTR VkBool32 VKAPI_CALL VulkanDevice::debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* /*userData*/) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        ENGINE_LOG_ERROR("[VK-VAL] %s", data->pMessage);
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ENGINE_LOG_WARN("[VK-VAL] %s", data->pMessage);
    }
    return VK_FALSE;
}

void VulkanDevice::setObjName(VkObjectType type, uint64_t handle, const char* name) {
    if (!_debugUtilsEnabled || !_setNameFn || handle == 0) return;
    VkDebugUtilsObjectNameInfoEXT oi{VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT};
    oi.objectType   = type;
    oi.objectHandle = handle;
    oi.pObjectName  = name;
    _setNameFn(_device, &oi);
}

void VulkanDevice::createSurface() {
    VK_CHECK(glfwCreateWindowSurface(_instance, _window, nullptr, &_surface));
}

void VulkanDevice::pickPhysicalDevice() {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(_instance, &n, nullptr);
    if (n == 0) {
        ENGINE_LOG_ERROR("[VK] no physical device");
        std::abort();
    }
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(_instance, &n, devs.data());

    for (auto d : devs) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        // M3：选第一个支持 graphics 队列的设备（MoltenVK 单设备环境够用）
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &familyCount, families.data());
        for (uint32_t i = 0; i < familyCount; ++i) {
            VkBool32 presentOK = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, _surface, &presentOK);
            if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && presentOK) {
                _physicalDevice = d;
                _queueFamily    = i;
                _adapterName    = props.deviceName;
                return;
            }
        }
    }
    ENGINE_LOG_ERROR("[VK] no suitable physical device");
    std::abort();
}

void VulkanDevice::createLogicalDevice() {
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = _queueFamily;
    qci.queueCount       = 1;
    qci.pQueuePriorities  = &priority;

    // 设备扩展：swapchain 必须；portability_subset 存在时规范要求必须启用（MoltenVK）
    std::vector<const char*> devExts;
    uint32_t devExtCount = 0;
    vkEnumerateDeviceExtensionProperties(_physicalDevice, nullptr, &devExtCount, nullptr);
    std::vector<VkExtensionProperties> avail(devExtCount);
    vkEnumerateDeviceExtensionProperties(_physicalDevice, nullptr, &devExtCount, avail.data());
    devExts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    bool hasIndexing = false;
    bool hasMaintenance1 = false;
    bool hasMaintenance3 = false;
    for (auto& e : avail) {
        if (std::strcmp(e.extensionName, "VK_KHR_portability_subset") == 0) {
            devExts.push_back("VK_KHR_portability_subset");
        }
        if (std::strcmp(e.extensionName, "VK_EXT_descriptor_indexing") == 0) {
            hasIndexing = true;
        }
        if (std::strcmp(e.extensionName, "VK_KHR_maintenance1") == 0) {
            hasMaintenance1 = true;
        }
        if (std::strcmp(e.extensionName, "VK_KHR_maintenance3") == 0) {
            hasMaintenance3 = true;
        }
    }
    if (hasIndexing && hasMaintenance3) {
        devExts.push_back("VK_KHR_maintenance3");   // descriptor_indexing 的依赖
    }
    // B3a：负高度 viewport（GL 约定 y 向上，见 VkCommandList::beginRenderPass）。
    // 实例按 1.0 请求（规避 loader/MVK 版本协商问题），验证层按
    // min(实例, 设备) 版本判定，maintenance1 虽在 1.1 核心化仍需显式启用
    if (hasMaintenance1) {
        devExts.push_back("VK_KHR_maintenance1");
    } else {
        ENGINE_LOG_WARN("[VK] VK_KHR_maintenance1 unavailable: negative-viewport y-flip not spec-legal");
    }
    // A1/M4.2c: bindTexture 需要 update-after-bind 描述符更新
    if (hasIndexing) {
        devExts.push_back("VK_EXT_descriptor_indexing");
        _descriptorIndexing = true;
    }

    // B1：Dynamic Rendering（离屏 RenderTarget / resize 不重建 PSO / ImGui 动态渲染的基础）
    _deviceApiVersion = _caps.props.apiVersion;
    bool hasDynamicRenderingExt = _caps.hasDynamicRendering;
    bool isVulkan13 = _deviceApiVersion >= VK_API_VERSION_1_3;
    if (isVulkan13 || hasDynamicRenderingExt) {
        _dynamicRendering = true;
        if (!isVulkan13) {
            // 1.2 设备走扩展路径：dynamic rendering 依赖的扩展必须一并启用
            //（规范依赖链：format_feature_flags2 / maintenance2 → create_renderpass2
            //  → depth_stencil_resolve → dynamic_rendering）
            const char* dynDeps[] = {
                "VK_KHR_maintenance2",
                "VK_KHR_multiview",
                "VK_KHR_format_feature_flags2",
                "VK_KHR_create_renderpass2",
                "VK_KHR_depth_stencil_resolve",
            };
            for (const char* dep : dynDeps) {
                if (_caps.hasExtension(dep)) {
                    devExts.push_back(dep);
                } else {
                    ENGINE_LOG_WARN("[VK] dynamic rendering dependency missing: %s", dep);
                }
            }
            devExts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
        }
    } else {
        ENGINE_LOG_ERROR("[VK] dynamic rendering unavailable (api=%u.%u.%u, ext=%d) — B1 requires it; abort",
                         VK_VERSION_MAJOR(_deviceApiVersion), VK_VERSION_MINOR(_deviceApiVersion),
                         VK_VERSION_PATCH(_deviceApiVersion), hasDynamicRenderingExt ? 1 : 0);
        std::abort();
    }

    // A1/M4.2c: UPDATE_AFTER_BIND 特性（bindTexture 需在 draw 期间原位更新描述符）
    VkPhysicalDeviceVulkan12Features vk12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    vk12.descriptorIndexing = VK_TRUE;
    vk12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    vk12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;

    // B1：dynamic rendering 特性（1.3 core 用 Vulkan13Features；1.2 + 扩展用 KHR struct）
    VkPhysicalDeviceVulkan13Features vk13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    vk13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    dynFeat.dynamicRendering = VK_TRUE;
    void* featureChain = nullptr;
    if (isVulkan13) {
        vk12.pNext = &vk13;           // vk12 → vk13
        featureChain = &vk12;
    } else {
        vk12.pNext = &dynFeat;        // vk12 → KHR dynamic rendering
        featureChain = &vk12;
    }

    // B3：启用 samplerAnisotropy（各向异性过滤，纹理采样描述符 SamplerDesc 使用；
    // 物理设备不支持时保持关闭，createSampler 会跳过 anisotropyEnable）
    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.samplerAnisotropy = _caps.features.samplerAnisotropy;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &qci;
    ci.enabledExtensionCount   = static_cast<uint32_t>(devExts.size());
    ci.ppEnabledExtensionNames = devExts.data();
    ci.pEnabledFeatures        = &enabledFeatures;
    ci.pNext                   = featureChain;
    VK_CHECK(vkCreateDevice(_physicalDevice, &ci, nullptr, &_device));
    vkGetDeviceQueue(_device, _queueFamily, 0, &_queue);
    // B1：KHR 入口点（1.3 设备驱动同样导出已提升扩展的 KHR 别名，统一走它）
    _vkBeginRendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(
        vkGetDeviceProcAddr(_device, "vkCmdBeginRenderingKHR"));
    _vkEndRendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(
        vkGetDeviceProcAddr(_device, "vkCmdEndRenderingKHR"));
    if (_dynamicRendering && (!_vkBeginRendering || !_vkEndRendering)) {
        ENGINE_LOG_ERROR("[VK] vkCmdBeginRenderingKHR not resolvable");
        std::abort();
    }
    setObjName(VK_OBJECT_TYPE_DEVICE, reinterpret_cast<uint64_t>(_device), "RHI_VulkanDevice");
    setObjName(VK_OBJECT_TYPE_QUEUE, reinterpret_cast<uint64_t>(_queue), "RHI_GraphicsQueue");
}

void VulkanDevice::createSwapchainObjects() {
    int fbW = 0, fbH = 0;
    glfwGetFramebufferSize(_window, &fbW, &fbH);

    // surface 能力
    VkSurfaceCapabilitiesKHR caps;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(_physicalDevice, _surface, &caps);
    _swapchainExtent = caps.currentExtent;
    if (_swapchainExtent.width == UINT32_MAX) {
        _swapchainExtent.width  = std::clamp(static_cast<uint32_t>(fbW),  caps.minImageExtent.width,  caps.maxImageExtent.width);
        _swapchainExtent.height = std::clamp(static_cast<uint32_t>(fbH), caps.minImageExtent.height, caps.maxImageExtent.height);
    }

    // surface 格式：优先 BGRA8_UNORM，其次 RGBA8_UNORM
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &fmtCount, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(_physicalDevice, _surface, &fmtCount, formats.data());
    _swapchainFormat = formats[0].format;
    for (auto& f : formats) {
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            _swapchainFormat = f.format;
            break;
        }
    }

    uint32_t imageCount = std::max(2u, caps.minImageCount);
    if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) imageCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface          = _surface;
    sci.minImageCount    = imageCount;
    sci.imageFormat      = _swapchainFormat;
    sci.imageColorSpace  = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    sci.imageExtent      = _swapchainExtent;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;  // TRANSFER_SRC 供 RHI_PROBE 回读验证
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;   // vsync（M3 固定）
    sci.clipped          = VK_TRUE;
    VK_CHECK(vkCreateSwapchainKHR(_device, &sci, nullptr, &_swapchain));
    setObjName(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<uint64_t>(_swapchain), "RHI_Swapchain");

    // image views
    uint32_t n = 0;
    vkGetSwapchainImagesKHR(_device, _swapchain, &n, nullptr);
    _swapchainImages.resize(n);
    vkGetSwapchainImagesKHR(_device, _swapchain, &n, _swapchainImages.data());
    _swapchainViews.resize(n);
    for (uint32_t i = 0; i < n; ++i) {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image    = _swapchainImages[i];
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = _swapchainFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(_device, &vci, nullptr, &_swapchainViews[i]));
        char name[64];
        std::snprintf(name, sizeof(name), "RHI_SwapchainImage_%u", i);
        setObjName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(_swapchainImages[i]), name);
        std::snprintf(name, sizeof(name), "RHI_SwapchainView_%u", i);
        setObjName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(_swapchainViews[i]), name);
    }

    // per-image render-done semaphores（随 swapchain 生命周期重建）
    _semRenderDone.resize(n, VK_NULL_HANDLE);
    VkSemaphoreCreateInfo rsci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (uint32_t i = 0; i < n; ++i) {
        VK_CHECK(vkCreateSemaphore(_device, &rsci, nullptr, &_semRenderDone[i]));
        char name[64];
        std::snprintf(name, sizeof(name), "RHI_RenderDone_%u", i);
        setObjName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(_semRenderDone[i]), name);
    }

    // B1：dynamic rendering 取代 renderpass + framebuffers。
    // 布局跟踪初始化：acquire 后首帧为 UNDEFINED，present 后由 recordPresentBarrier 置回 PRESENT_SRC
    _swapchainLayouts.assign(n, VK_IMAGE_LAYOUT_UNDEFINED);

    _depthFormat = findSupportedDepthFormat(_physicalDevice);

    // 深度图像（单一深度缓冲，frame-in-flight=1 下共享合法）
    {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = _depthFormat;
        ici.extent        = {_swapchainExtent.width, _swapchainExtent.height, 1};
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VK_CHECK(vkCreateImage(_device, &ici, nullptr, &_depthImage));
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(_device, _depthImage, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(_physicalDevice, req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK(vkAllocateMemory(_device, &mai, nullptr, &_depthMemory));
        VK_CHECK(vkBindImageMemory(_device, _depthImage, _depthMemory, 0));

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image    = _depthImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = _depthFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VK_CHECK(vkCreateImageView(_device, &vci, nullptr, &_depthView));
        setObjName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(_depthView), "RHI_DepthView");
        setObjName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(_depthImage), "RHI_DepthImage");
    }
    _depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    _needsRecreate = false;
}

void VulkanDevice::destroySwapchainObjects() {
    for (auto s : _semRenderDone)   if (s)              vkDestroySemaphore(_device, s, nullptr);
    _semRenderDone.clear();
    for (auto v : _swapchainViews)  if (v)              vkDestroyImageView(_device, v, nullptr);
    if (_depthView)   vkDestroyImageView(_device, _depthView, nullptr);
    if (_depthImage)  vkDestroyImage(_device, _depthImage, nullptr);
    if (_depthMemory) vkFreeMemory(_device, _depthMemory, nullptr);
    _depthView = VK_NULL_HANDLE; _depthImage = VK_NULL_HANDLE; _depthMemory = VK_NULL_HANDLE;
    if (_swapchain)  vkDestroySwapchainKHR(_device, _swapchain, nullptr);
    _swapchainLayouts.clear();
    _swapchainViews.clear();
    _swapchainImages.clear();
    _swapchain = VK_NULL_HANDLE;
}

void VulkanDevice::createCommandObjects() {
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = _queueFamily;
    pci.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VK_CHECK(vkCreateCommandPool(_device, &pci, nullptr, &_commandPool));

    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = _commandPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VK_CHECK(vkAllocateCommandBuffers(_device, &ai, &_commandBuffer));
    setObjName(VK_OBJECT_TYPE_COMMAND_POOL, reinterpret_cast<uint64_t>(_commandPool), "RHI_CommandPool");
    setObjName(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<uint64_t>(_commandBuffer), "RHI_MainCmdBuffer");
}

void VulkanDevice::createSyncObjects() {
    VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VK_CHECK(vkCreateSemaphore(_device, &sci, nullptr, &_semImageAvailable));

    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // 首帧可直接等待
    VK_CHECK(vkCreateFence(_device, &fci, nullptr, &_fenceInFlight));
    setObjName(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(_semImageAvailable), "RHI_ImageAvailable");
    // _semRenderDone 为 per-image，在 createSwapchainObjects 中创建
    setObjName(VK_OBJECT_TYPE_FENCE, reinterpret_cast<uint64_t>(_fenceInFlight), "RHI_InFlightFence");
}

// ============================================================
// A3: Pipeline Cache 持久化
// ============================================================
// 缓存文件头校验：跨驱动/跨设备的缓存可能不兼容。规范要求驱动忽略不兼容数据，
// 但主动校验可避免验证层告警，也能在日志里说明"为什么没加速"。
static bool pipelineCacheHeaderMatches(const std::vector<char>& data,
                                       const VkPhysicalDeviceProperties& props) {
    if (data.size() < sizeof(VkPipelineCacheHeaderVersionOne)) return false;
    VkPipelineCacheHeaderVersionOne h{};
    std::memcpy(&h, data.data(), sizeof(h));
    return h.headerVersion == VK_PIPELINE_CACHE_HEADER_VERSION_ONE
        && h.vendorID == props.vendorID
        && h.deviceID == props.deviceID
        && std::memcmp(h.pipelineCacheUUID, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
}

void VulkanDevice::createPipelineCache() {
    const char* env = std::getenv("RHI_VK_PIPELINE_CACHE");
    if (env && std::strcmp(env, "0") == 0) {
        ENGINE_LOG_INFO("[VK] pipeline cache disabled (RHI_VK_PIPELINE_CACHE=0)");
        return;
    }
    _pipelineCachePath = (env && *env) ? std::string(env)
                                       : _shaderRootPath + "pipeline_cache.bin";

    std::vector<char> initial;
    {
        std::ifstream f(_pipelineCachePath, std::ios::binary | std::ios::ate);
        if (f.is_open()) {
            const size_t size = static_cast<size_t>(f.tellg());
            initial.resize(size);
            f.seekg(0);
            f.read(initial.data(), static_cast<std::streamsize>(size));
            if (!pipelineCacheHeaderMatches(initial, _caps.props)) {
                ENGINE_LOG_INFO("[VK] pipeline cache ignored (driver/device mismatch): %s",
                                _pipelineCachePath.c_str());
                initial.clear();
            }
        }
    }

    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = initial.size();
    ci.pInitialData    = initial.empty() ? nullptr : initial.data();
    VkResult r = vkCreatePipelineCache(_device, &ci, nullptr, &_pipelineCache);
    if (r != VK_SUCCESS && !initial.empty()) {
        // 文件损坏或驱动拒绝：丢弃后重试（缓存只是优化，不应阻塞启动）
        ENGINE_LOG_WARN("[VK] pipeline cache load failed (%d), recreating empty", static_cast<int>(r));
        initial.clear();
        ci.initialDataSize = 0;
        ci.pInitialData    = nullptr;
        r = vkCreatePipelineCache(_device, &ci, nullptr, &_pipelineCache);
    }
    if (r != VK_SUCCESS) {
        ENGINE_LOG_WARN("[VK] pipeline cache unavailable (%d), pipelines compile uncached", static_cast<int>(r));
        _pipelineCache = VK_NULL_HANDLE;
        return;
    }
    setObjName(VK_OBJECT_TYPE_PIPELINE_CACHE, reinterpret_cast<uint64_t>(_pipelineCache), "RHI_PipelineCache");
    _pipelineCacheFromDisk = !initial.empty();
    _pipelineCacheLoadedBytes = initial.size();
    ENGINE_LOG_INFO("[VK] pipeline cache %s: %zu bytes (%s)",
                    _pipelineCacheFromDisk ? "loaded" : "empty",
                    _pipelineCacheLoadedBytes, _pipelineCachePath.c_str());
}

void VulkanDevice::savePipelineCache() {
    if (!_pipelineCache || _pipelineCachePath.empty()) return;

    size_t size = 0;
    if (vkGetPipelineCacheData(_device, _pipelineCache, &size, nullptr) != VK_SUCCESS || size == 0) {
        return;   // 本进程没有新建管线（全部命中缓存）时通常为空
    }
    std::vector<char> data(size);
    if (vkGetPipelineCacheData(_device, _pipelineCache, &size, data.data()) != VK_SUCCESS) return;
    data.resize(size);

    const std::string tmp = _pipelineCachePath + ".tmp";
    {
        std::error_code ec;
        auto parent = std::filesystem::path(tmp).parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f.is_open()) {
            ENGINE_LOG_WARN("[VK] cannot write pipeline cache: %s", tmp.c_str());
            return;
        }
        f.write(data.data(), static_cast<std::streamsize>(data.size()));
        if (!f) {
            ENGINE_LOG_WARN("[VK] pipeline cache write failed: %s", tmp.c_str());
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, _pipelineCachePath, ec);   // 先写临时文件再改名，避免崩溃留下半截缓存
    if (ec) {
        ENGINE_LOG_WARN("[VK] pipeline cache rename failed: %s", ec.message().c_str());
        return;
    }
    ENGINE_LOG_INFO("[VK] pipeline cache saved: %zu bytes -> %s", data.size(), _pipelineCachePath.c_str());
}

// ============================================================
// 帧循环
// ============================================================
bool VulkanDevice::beginFrameInternal() {
    VK_CHECK(vkWaitForFences(_device, 1, &_fenceInFlight, VK_TRUE, UINT64_MAX));

    VkResult r = vkAcquireNextImageKHR(_device, _swapchain, UINT64_MAX,
                                       _semImageAvailable, VK_NULL_HANDLE, &_imageIndex);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        _needsRecreate = true;
        return false;
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        VK_CHECK(r);
    }

    VK_CHECK(vkResetFences(_device, 1, &_fenceInFlight));
    VK_CHECK(vkResetCommandBuffer(_commandBuffer, 0));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(_commandBuffer, &bi));
    return true;
}

void VulkanDevice::deferGpuWork(std::function<void(VkCommandBuffer)> fn, std::function<void()> onComplete) {
    if (_frameActive) {
        _deferredGpu.push_back({std::move(fn), std::move(onComplete)});   // 帧内：延迟到下一帧开头
        return;
    }
    submitOneTime(std::move(fn), std::move(onComplete));                // 帧外：立即执行
}

void VulkanDevice::drainDeferredGpuWork() {
    if (_deferredGpu.empty()) return;
    // 用独立一次性提交执行（不动主命令缓冲：此时它可能仍处于 pending 状态）
    for (auto& w : _deferredGpu) submitOneTime(std::move(w.record), std::move(w.onComplete));
    _deferredGpu.clear();
}

void VulkanDevice::beginFrame() {
    drainDeferredGpuWork();   // 上一帧帧内延迟的纹理上传：现在帧外安全执行
    if (_needsRecreate) {
        vkDeviceWaitIdle(_device);
        destroySwapchainObjects();
        createSwapchainObjects();
        // B1：dynamic rendering 下管线不引用 renderPass/framebuffer，
        // swapchain 重建不再需要重建 PSO（原来 recreatePipelines 的唯一理由）
    }
    _frameSkipped = !beginFrameInternal();
    _frameActive  = !_frameSkipped;
}

void VulkanDevice::endFrame() {
    if (!_frameActive) { _frameSkipped = false; return; }
    _frameActive = false;

    // B1：dynamic rendering 无 renderpass finalLayout，present 前手动转回 PRESENT_SRC。
    // 仅在本帧真正渲染过（布局已被转到 COLOR_ATTACHMENT）时需要；空帧已是 PRESENT。
    recordPresentBarrier(_commandBuffer);
    VK_CHECK(vkEndCommandBuffer(_commandBuffer));

    if (_imageIndex >= _semRenderDone.size()) {
        ENGINE_LOG_ERROR("[VK] missing render-done semaphore for image %u", _imageIndex);
        return;
    }
    // 每个 swapchain image 使用独立的 render-done semaphore：
    // presentation 完成前不能复用同一个 binary semaphore（VUID-...-00067）
    VkSemaphore renderDone = _semRenderDone[_imageIndex];

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores    = &_semImageAvailable;
    si.pWaitDstStageMask  = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &_commandBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &renderDone;
    VK_CHECK(vkQueueSubmit(_queue, 1, &si, _fenceInFlight));

    // ---- RHI_PROBE=1：第 30 帧回读中心像素（M3 验证用，之后可移除）----
    // 必须在 present 之前执行：此时图像仍处于 acquired 状态，布局转换合法。
    // 单帧调试路径，允许同步 vkQueueWaitIdle。
    static int frameCount = 0;
    static bool probeEnabled = (std::getenv("RHI_PROBE") != nullptr);
    if (probeEnabled && ++frameCount == 30) {
        vkQueueWaitIdle(_queue);   // 主提交完成；图像处于 PRESENT_SRC，仍 acquired
        VkBuffer readback;
        VkDeviceMemory readbackMem;
        VkDeviceSize bufSize = _swapchainExtent.width * _swapchainExtent.height * 4;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size  = bufSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(_device, &bci, nullptr, &readback));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(_device, readback, &req);
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize  = req.size;
        mai.memoryTypeIndex = findMemoryType(_physicalDevice, req.memoryTypeBits,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(_device, &mai, nullptr, &readbackMem));
        VK_CHECK(vkBindBufferMemory(_device, readback, readbackMem, 0));

        // 主命令缓冲已完成，可 reset 后复用录制回读
        VkCommandBuffer cb = _commandBuffer;
        VK_CHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        VK_CHECK(vkBeginCommandBuffer(cb, &bi));
        VkImageMemoryBarrier toTransfer{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toTransfer.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toTransfer.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toTransfer.image = _swapchainImages[_imageIndex];
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toTransfer);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {_swapchainExtent.width, _swapchainExtent.height, 1};
        vkCmdCopyImageToBuffer(cb, _swapchainImages[_imageIndex],
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        // 拷贝后必须恢复 PRESENT_SRC，才能交给 present
        VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toPresent.dstAccessMask = 0;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = _swapchainImages[_imageIndex];
        toPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toPresent);
        VK_CHECK(vkEndCommandBuffer(cb));
        VkSubmitInfo s2{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s2.commandBufferCount = 1;
        s2.pCommandBuffers = &cb;
        VK_CHECK(vkQueueSubmit(_queue, 1, &s2, VK_NULL_HANDLE));
        VK_CHECK(vkQueueWaitIdle(_queue));

        void* mapped = nullptr;
        VK_CHECK(vkMapMemory(_device, readbackMem, 0, bufSize, 0, &mapped));
        const uint8_t* px = static_cast<const uint8_t*>(mapped);
        uint32_t cx = _swapchainExtent.width / 2, cy = _swapchainExtent.height * 2 / 5;
        const size_t off = (static_cast<size_t>(cy) * _swapchainExtent.width + cx) * 4;
        // swapchain 格式可能是 BGRA：打印原始字节 + 两种解释
        ENGINE_LOG_INFO("[VKPROBE] center bytes = %d,%d,%d,%d (format=%d)",
                    px[off], px[off+1], px[off+2], px[off+3], (int)_swapchainFormat);
        std::fflush(stdout);
        if (std::getenv("RHI_DUMP")) {   // 正式调试设施：导出整帧 PPM（跨平台临时目录）
            std::error_code ec;
            auto tmpDir = std::filesystem::temp_directory_path(ec);
            if (ec) tmpDir = ".";
            const std::string ppmPath = (tmpDir / "frame_vk.ppm").string();
            FILE* fp = fopen(ppmPath.c_str(), "wb");
            if (fp) {
                std::fprintf(fp, "P6\n%u %u\n255\n", _swapchainExtent.width, _swapchainExtent.height);
                // B3a 完成后 3D 内容已是正立，按屏幕行序直接导出
                for (uint32_t y = 0; y < _swapchainExtent.height; ++y) {
                    const uint8_t* row = px + static_cast<size_t>(y) * _swapchainExtent.width * 4;
                    for (uint32_t x = 0; x < _swapchainExtent.width; ++x) {
                        const uint8_t* px4 = row + static_cast<size_t>(x) * 4;
                        const uint8_t rgb[3] = { px4[2], px4[1], px4[0] };   // BGRA → RGB 真色
                        fwrite(rgb, 1, 3, fp);
                    }
                }
                fclose(fp);
            }
        }
        vkUnmapMemory(_device, readbackMem);
        vkDestroyBuffer(_device, readback, nullptr);
        vkFreeMemory(_device, readbackMem, nullptr);
        std::fflush(stdout);
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &renderDone;
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &_swapchain;
    pi.pImageIndices      = &_imageIndex;
    VkResult r = vkQueuePresentKHR(_queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        _needsRecreate = true;
    }
}

void VulkanDevice::onResize(int width, int height) {
    // 尺寸未变则忽略（应用启动时 onWindowSizeChanged 传来的尺寸与创建时一致，
    // 若此时重建 swapchain，会销毁管线引用的 renderPass 导致绘制崩溃）
    if (width > 0 && height > 0 &&
        (static_cast<uint32_t>(width)  != _swapchainExtent.width ||
         static_cast<uint32_t>(height) != _swapchainExtent.height)) {
        _needsRecreate = true;
    }
}

void VulkanDevice::waitIdle() {
    if (_device != VK_NULL_HANDLE) vkDeviceWaitIdle(_device);
}

// ============================================================
// 资源创建
// ============================================================
BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc) {
    VkBufferRes res;
    res.size = desc.size;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = desc.size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
              | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(_device, &bci, nullptr, &res.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(_device, res.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(_physicalDevice, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(_device, &mai, nullptr, &res.memory));
    VK_CHECK(vkBindBufferMemory(_device, res.buffer, res.memory, 0));
    VK_CHECK(vkMapMemory(_device, res.memory, 0, desc.size, 0, &res.mapped));
    if (desc.initialData && desc.size > 0) {
        std::memcpy(res.mapped, desc.initialData, desc.size);
    }

    _buffers.push_back(res);
    BufferHandle h; h.id = static_cast<uint32_t>(_buffers.size());
    char name[48];
    std::snprintf(name, sizeof(name), "RHI_Buffer_%u", h.id);
    setObjName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(res.buffer), name);
    return h;
}

void VulkanDevice::updateBuffer(BufferHandle buffer, const void* data,
                            uint32_t size, uint32_t offset) {
    if (!buffer.valid() || !data || buffer.id > _buffers.size()) return;
    VkBufferRes& b = _buffers[buffer.id - 1];
    if (!b.mapped || offset + size > b.size) return;   // 已销毁 / 越界保护
    std::memcpy(static_cast<uint8_t*>(b.mapped) + offset, data, size);
}

// ---- v2 对象级销毁（B0）----
void VulkanDevice::destroyBuffer(BufferHandle h) {
    if (!h.valid() || h.id > _buffers.size()) return;
    VkBufferRes& b = _buffers[h.id - 1];
    if (!b.buffer) return;
    vkDestroyBuffer(_device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(_device, b.memory, nullptr);   // free 隐含 unmap
    b = VkBufferRes{};
}

void VulkanDevice::destroyTexture(TextureHandle h) {
    if (!h.valid() || h.id > _textures.size()) return;
    VkTextureRes& t = _textures[h.id - 1];
    if (!t.image) return;
    if (t.sampler) vkDestroySampler(_device, t.sampler, nullptr);
    if (t.view)    vkDestroyImageView(_device, t.view, nullptr);
    vkDestroyImage(_device, t.image, nullptr);
    if (t.memory)  vkFreeMemory(_device, t.memory, nullptr);
    t = VkTextureRes{};
}

void VulkanDevice::destroyPipeline(PipelineHandle h) {
    if (!h.valid() || h.id > _pipelines.size()) return;
    VkPipelineRes& p = _pipelines[h.id - 1];
    if (p.pipeline) vkDestroyPipeline(_device, p.pipeline, nullptr);
    p = VkPipelineRes{};
    p.alive = false;   // 槽位保留（id 不复用）；swapchain 重建不复活
}

void VulkanDevice::destroyUniformBlock(UniformBlockHandle h) {
    if (!h.valid() || h.id > _uniformBlocks.size()) return;
    if (h.id == _defaultUniformBlock.id) {
        ENGINE_LOG_WARN_ONCE("[VK] destroyUniformBlock: device default block is device-owned, ignored");
        return;
    }
    VkUniformBlockRes& b = _uniformBlocks[h.id - 1];
    if (!b.buffer) return;
    vkDestroyBuffer(_device, b.buffer, nullptr);
    if (b.memory) vkFreeMemory(_device, b.memory, nullptr);   // 描述符集随 pool 回收（无 FREE_DESCRIPTOR_SET_BIT）
    b = VkUniformBlockRes{};
}

TextureHandle VulkanDevice::createTexture(const TextureDesc& desc) {
    VkTextureRes t;
    t.width = desc.width; t.height = desc.height;
    const uint32_t layerCount = desc.cubemap ? 6u : 1u;
    const VkFormat vkFmt = vkFormatOf(desc.format);
    const uint32_t bpp = formatBytesPerPixel(desc.format);
    if (bpp == 0) {
        ENGINE_LOG_ERROR("[VK] unsupported format bpp (%d)", static_cast<int>(desc.format));
        return {};
    }
    // mip 级数：显式指定 > 0 用之；否则只有 generateMips 时分配完整链
    uint32_t mipLevels = desc.mipLevels > 0 ? desc.mipLevels : 1;
    bool generateMips = desc.generateMips && desc.initialData;
    if (desc.mipLevels == 0 && desc.generateMips) {
        mipLevels = fullMipChainLevels(desc.width, desc.height);
    }
    if (generateMips && mipLevels > 1 && !formatBlitSupported(_physicalDevice, vkFmt)) {
        ENGINE_LOG_WARN_ONCE("[VK] blit unsupported for format %d, mipmap generation disabled",
                             static_cast<int>(vkFmt));
        generateMips = false;
        mipLevels = desc.mipLevels > 0 ? desc.mipLevels : 1;
    }

    // ---- image（DEVICE_LOCAL + OPTIMAL）----
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = vkFmt;
    ici.extent        = {desc.width, desc.height, 1};
    ici.mipLevels     = mipLevels;
    ici.arrayLayers   = layerCount;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    if (desc.cubemap) ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                      | (desc.initialData ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0)
                      | (generateMips ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0);
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK(vkCreateImage(_device, &ici, nullptr, &t.image));

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(_device, t.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(_physicalDevice, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VK_CHECK(vkAllocateMemory(_device, &mai, nullptr, &t.memory));
    VK_CHECK(vkBindImageMemory(_device, t.image, t.memory, 0));

    // ---- 数据上传：staging → 拷贝 + （可选）mipmap blit + 布局转换 ----
    // 可能在 render pass 活跃的帧内被调用：GPU 工作经 deferGpuWork 延迟到下一帧
    // 开头执行；数据由 lambda 持有（调用方的 initialData 生命周期未知）
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(desc.width) * desc.height * bpp * layerCount;
    if (desc.initialData && bytes > 0) {
        auto dataCopy = std::make_shared<std::vector<unsigned char>>(
            static_cast<const unsigned char*>(desc.initialData),
            static_cast<const unsigned char*>(desc.initialData) + static_cast<size_t>(bytes));
        // staging 在宿主阶段创建并填充（宿主操作可安全执行于帧内）；
        // 拷贝命令延迟录制；staging 的销毁在 GPU 执行完成后由完成回调执行
        VkBuffer staging;
        VkDeviceMemory stagingMem;
        VkBufferCreateInfo sbi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        sbi.size  = bytes;
        sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        sbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(_device, &sbi, nullptr, &staging));
        VkMemoryRequirements req2;
        vkGetBufferMemoryRequirements(_device, staging, &req2);
        VkMemoryAllocateInfo mai2{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai2.allocationSize  = req2.size;
        mai2.memoryTypeIndex = findMemoryType(_physicalDevice, req2.memoryTypeBits,
                                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkAllocateMemory(_device, &mai2, nullptr, &stagingMem));
        VK_CHECK(vkBindBufferMemory(_device, staging, stagingMem, 0));
        void* mapped;
        VK_CHECK(vkMapMemory(_device, stagingMem, 0, bytes, 0, &mapped));
        std::memcpy(mapped, dataCopy->data(), static_cast<size_t>(bytes));
        vkUnmapMemory(_device, stagingMem);

        deferGpuWork([this, t, staging, stagingMem, w = desc.width, h = desc.height,
                      bpp, layerCount, mipLevels, generateMips](VkCommandBuffer cb) {

            VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            bar.srcAccessMask = 0;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = t.image;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layerCount};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &bar);
            std::vector<VkBufferImageCopy> regions(layerCount);
            for (uint32_t face = 0; face < layerCount; ++face) {
                regions[face] = {};
                regions[face].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
                regions[face].imageExtent = {w, h, 1};
                regions[face].bufferOffset = static_cast<VkDeviceSize>(face) * w * h * (bpp);
            }
            vkCmdCopyBufferToImage(cb, staging, t.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   layerCount, regions.data());

            if (generateMips && mipLevels > 1) {
                // B3：逐级 blit 生成 mip 链（每层独立；第 i-1 级 DST→SRC 后作源，
                // 双线性缩半写入第 i 级）。不支持的格式已在创建期回退单级。
                for (uint32_t layer = 0; layer < layerCount; ++layer) {
                    for (uint32_t i = 1; i < mipLevels; ++i) {
                        VkImageMemoryBarrier srcBar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        srcBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        srcBar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        srcBar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        srcBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        srcBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        srcBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        srcBar.image = t.image;
                        srcBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, layer, 1};
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                             0, nullptr, 0, nullptr, 1, &srcBar);

                        VkImageBlit blit{};
                        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, layer, 1};
                        blit.srcOffsets[1] = { static_cast<int32_t>(w >> (i - 1)),
                                               static_cast<int32_t>(h >> (i - 1)), 1 };
                        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, layer, 1};
                        blit.dstOffsets[1]  = { static_cast<int32_t>(std::max(1u, w >> i)),
                                                static_cast<int32_t>(std::max(1u, h >> i)), 1 };
                        vkCmdBlitImage(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &blit, VK_FILTER_LINEAR);
                    }
                }
                // 全链 → SHADER_READ：末级在 DST，其余在 SRC，分两段转换
                bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels - 1, 0, layerCount};
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &bar);
                bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevels - 1, 1, 0, layerCount};
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &bar);
            } else {
                bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &bar);
            }
        }, [this, staging, stagingMem]() {
            vkDestroyBuffer(_device, staging, nullptr);
            vkFreeMemory(_device, stagingMem, nullptr);
        });
    }
    // ---- view + sampler ----
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image    = t.image;
    vci.viewType = desc.cubemap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = vkFmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, layerCount};
    VK_CHECK(vkCreateImageView(_device, &vci, nullptr, &t.view));

    // B3：采样参数由 TextureDesc::sampler 驱动（契约 SamplerDesc）
    const SamplerDesc& sd = desc.sampler;
    auto filterOf = [](FilterMode m) {
        return m == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    };
    auto wrapOf = [](WrapMode m) {
        switch (m) {
            case WrapMode::Repeat:      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case WrapMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case WrapMode::Mirror:      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter        = filterOf(sd.magFilter);
    sci.minFilter        = filterOf(sd.minFilter);
    sci.mipmapMode       = sd.mipFilter == FilterMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                               : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // cubemap：Vulkan 要求地址模式为 CLAMP_TO_EDGE/BORDER/MIRRORED_CLAMP
    // （REPEAT 采样 cube 属违规），与 GL 天空盒路径的 CLAMP_TO_EDGE 对齐
    sci.addressModeU     = desc.cubemap ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : wrapOf(sd.wrapU);
    sci.addressModeV     = desc.cubemap ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : wrapOf(sd.wrapV);
    sci.addressModeW     = desc.cubemap ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : wrapOf(sd.wrapW);
    sci.mipLodBias       = 0.0f;
    sci.anisotropyEnable = VK_FALSE;
    sci.maxAnisotropy    = 1.0f;
    if (sd.maxAnisotropy > 1.0f && _caps.features.samplerAnisotropy == VK_TRUE) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy    = std::min(sd.maxAnisotropy, _caps.props.limits.maxSamplerAnisotropy);
    }
    sci.compareEnable    = sd.compareEnable ? VK_TRUE : VK_FALSE;
    // 深度比较采样（阴影 PCF 预留）：Reversed-Z 时代取 GREATER
    sci.compareOp        = VK_COMPARE_OP_GREATER;
    sci.minLod           = sd.minLod;
    sci.maxLod           = sd.maxLod;
    sci.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VK_CHECK(vkCreateSampler(_device, &sci, nullptr, &t.sampler));

    _textures.push_back(t);
    char tname[48];
    std::snprintf(tname, sizeof(tname), "RHI_Texture_%u", static_cast<uint32_t>(_textures.size()));
    setObjName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(t.image), tname);
    setObjName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(t.view), tname);
    setObjName(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(t.sampler), tname);
    TextureHandle h; h.id = static_cast<uint32_t>(_textures.size());
    return h;
}

PipelineHandle VulkanDevice::createPipeline(const GraphicsPipelineDesc& desc) {
    VkPipelineRes res;

    // ---- 着色器模块：从 shaders_vk/ 读取 <name>.spv ----
    std::vector<VkPipelineShaderStageCreateInfo> stages;
    std::vector<VkShaderModule> modules;
    for (const auto& s : desc.shaders) {
        const std::string path = _shaderRootPath + s.path + ".spv";
        std::vector<char> code = readSpirvFile(path);
        if (code.empty()) {
            ENGINE_LOG_ERROR("[VK] missing SPIR-V for pipeline");
            return {};
        }
        VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mci.codeSize = code.size();
        mci.pCode    = reinterpret_cast<const uint32_t*>(code.data());
        VkShaderModule mod;
        VK_CHECK(vkCreateShaderModule(_device, &mci, nullptr, &mod));
        modules.push_back(mod);

        VkPipelineShaderStageCreateInfo st{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        st.stage  = (s.stage == ShaderStage::Vertex) ? VK_SHADER_STAGE_VERTEX_BIT
                                                     : VK_SHADER_STAGE_FRAGMENT_BIT;
        st.module = mod;
        st.pName  = "main";
        stages.push_back(st);
    }

    // ---- 顶点输入 ----
    // ---- 顶点输入：binding0 = 顶点率，binding1 = 实例率（有实例化属性时）----
    bool hasInstanced = false;
    for (const auto& a : desc.vertexLayout.attributes) hasInstanced |= a.instanced;
    VkVertexInputBindingDescription binds[2] = {};
    binds[0] = {0, desc.vertexLayout.stride, VK_VERTEX_INPUT_RATE_VERTEX};
    binds[1] = {1, desc.vertexLayout.instanceStride, VK_VERTEX_INPUT_RATE_INSTANCE};
    std::vector<VkVertexInputAttributeDescription> attrs;
    for (const auto& a : desc.vertexLayout.attributes) {
        VkVertexInputAttributeDescription va{};
        va.location = a.location;
        va.binding  = a.instanced ? 1 : 0;
        va.offset   = a.offset;
        switch (a.format) {
            case VertexFormat::Float1: va.format = VK_FORMAT_R32_SFLOAT; break;
            case VertexFormat::Float2: va.format = VK_FORMAT_R32G32_SFLOAT; break;
            case VertexFormat::Float3: va.format = VK_FORMAT_R32G32B32_SFLOAT; break;
            case VertexFormat::Float4: va.format = VK_FORMAT_R32G32B32A32_SFLOAT; break;
        }
        attrs.push_back(va);
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount   = hasInstanced ? 2 : 1;
    vi.pVertexBindingDescriptions      = binds;
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrs.size());
    vi.pVertexAttributeDescriptions    = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    switch (desc.primitive) {
        case PrimitiveType::Triangles:     ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
        case PrimitiveType::Lines:         ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        case PrimitiveType::TriangleStrip: ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    }

    // ---- 动态 viewport/scissor ----
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount  = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode    = (desc.cull == CullMode::Back) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    // B3a：负高度 viewport 把 NDC y 镜像到屏幕，绕序相对 GL 约定翻转；
    // GL 网格（CCW 正面）在此后端声明 CLOCKWISE 才能正确剔除背面
    rs.frontFace   = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth   = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // M4：render pass 已有深度附件，深度状态由管线描述符驱动
    ds.depthTestEnable  = desc.depth.depthTest  ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = desc.depth.depthWrite ? VK_TRUE : VK_FALSE;
    // B3b Reversed-Z：深度语义反转（近=1/远=0，清屏 0，见 VkCommandList::setUniform），
    // 比较算子相应改为 GREATER 族；depthLessEqual（天空盒远景）映射 GREATER_OR_EQUAL
    ds.depthCompareOp   = desc.depth.depthLessEqual ? VK_COMPARE_OP_GREATER_OR_EQUAL
                                                    : VK_COMPARE_OP_GREATER;

    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                       | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    if (desc.blend.blendEnable) {
        att.blendEnable    = VK_TRUE;
        att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        att.colorBlendOp        = VK_BLEND_OP_ADD;
        att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    }
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments    = &att;

    VkDynamicState dynamics[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 2;
    dyn.pDynamicStates    = dynamics;

    // ---- pipeline layout：固定 push constant 布局（与 shaders_vk GLSL 一致）----
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = kPushTotalSize;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pushRange;
    plci.setLayoutCount         = 1;
    plci.pSetLayouts            = &_texSetLayout;
    // 固定布局只创建一次，所有管线共用（此前每管线创建一个 layout 且从未销毁 → 泄漏）
    if (_pipelineLayout == VK_NULL_HANDLE) {
        VK_CHECK(vkCreatePipelineLayout(_device, &plci, nullptr, &_pipelineLayout));
        setObjName(VK_OBJECT_TYPE_PIPELINE_LAYOUT, reinterpret_cast<uint64_t>(_pipelineLayout), "RHI_PipelineLayout");
    }

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount          = static_cast<uint32_t>(stages.size());
    gpi.pStages             = stages.data();
    gpi.pVertexInputState   = &vi;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState      = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState   = &ms;
    gpi.pDepthStencilState  = &ds;
    gpi.pColorBlendState    = &cb;
    gpi.pDynamicState       = &dyn;
    gpi.layout              = _pipelineLayout;
    // B1：dynamic rendering —— 管线经 PipelineRenderingCreateInfo 绑定附件格式，
    // 不再引用 VkRenderPass → swapchain 重建无需重建 PSO
    VkPipelineRenderingCreateInfo prci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    prci.colorAttachmentCount    = 1;
    prci.pColorAttachmentFormats = &_swapchainFormat;
    prci.depthAttachmentFormat   = _depthFormat;
    gpi.pNext               = &prci;
    gpi.renderPass          = VK_NULL_HANDLE;
    gpi.subpass             = 0;
    VK_CHECK(vkCreateGraphicsPipelines(_device, _pipelineCache, 1, &gpi, nullptr, &res.pipeline));
    {
        char pname[48];
        std::snprintf(pname, sizeof(pname), "RHI_Pipeline_%u", static_cast<uint32_t>(_pipelines.size() + 1));
        setObjName(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(res.pipeline), pname);
    }

    for (auto m : modules) vkDestroyShaderModule(_device, m, nullptr);

    _pipelines.push_back(std::move(res));
    PipelineHandle h; h.id = static_cast<uint32_t>(_pipelines.size());
    return h;
}

// ============================================================
// 查询 / 其他
// ============================================================
VkImageView VulkanDevice::currentColorView() const {
    if (_imageIndex < _swapchainViews.size()) return _swapchainViews[_imageIndex];
    return VK_NULL_HANDLE;
}

// B1：beginRenderPass 前的附件布局屏障。
// color：PRESENT_SRC（或首帧 UNDEFINED）→ COLOR_ATTACHMENT_OPTIMAL；
// depth：UNDEFINED（首帧，loadOp CLEAR 丢弃旧内容）→ DEPTH_ATTACHMENT_OPTIMAL，
// 之后保持不变，每帧补一条同布局的访问链屏障（前帧写 → 本帧读写）。
void VulkanDevice::recordBeginPassBarriers(VkCommandBuffer cb, bool clearColor, bool clearDepth) {
    if (_imageIndex >= _swapchainLayouts.size()) return;
    VkImageLayout colorOld = _swapchainLayouts[_imageIndex];
    if (colorOld != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask = 0;   // PRESENT/UNDEFINED 无需 flush
        bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.oldLayout = colorOld;
        bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = _swapchainImages[_imageIndex];
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &bar);
        _swapchainLayouts[_imageIndex] = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    if (_depthLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL || clearDepth) {
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                          | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bar.oldLayout = _depthLayout;   // UNDEFINED 首帧 → 一次性转换；之后同布局访问链
        bar.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = _depthImage;
        bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &bar);
        _depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
}

// B1：present 前把当前 image 转回 PRESENT_SRC（仅当本帧渲染过）。
void VulkanDevice::recordPresentBarrier(VkCommandBuffer cb) {
    if (_imageIndex >= _swapchainLayouts.size()) return;
    if (_swapchainLayouts[_imageIndex] != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) return;
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    bar.dstAccessMask = 0;
    bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = _swapchainImages[_imageIndex];
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &bar);
    _swapchainLayouts[_imageIndex] = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
}

bool VulkanDevice::nativeHandles(NativeHandles& out) const {
    out.instance       = reinterpret_cast<uint64_t>(_instance);
    out.physicalDevice = reinterpret_cast<uint64_t>(_physicalDevice);
    out.device         = reinterpret_cast<uint64_t>(_device);
    out.queue          = reinterpret_cast<uint64_t>(_queue);
    out.renderPass     = 0;   // B1：dynamic rendering，无 renderpass（ImGui 走 UseDynamicRendering）
    out.queueFamily    = _queueFamily;
    out.minImageCount  = 2;
    out.imageCount     = static_cast<uint32_t>(_swapchainImages.size());
    out.colorFormat    = static_cast<uint32_t>(_swapchainFormat);
    out.depthFormat    = static_cast<uint32_t>(_depthFormat);
    return true;
}

IRHICommandList* VulkanDevice::commandList() {
    return _commandList.get();
}

void VulkanDevice::createDescriptorObjects() {
    // set layout：binding0 = 绘制级 uniform UBO；bindings 1..4 = 采样器槽 0..3
    VkDescriptorSetLayoutBinding bindings[5];
    bindings[0] = {};
    bindings[0].binding         = 0;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (uint32_t i = 0; i < 4; ++i) {
        bindings[i + 1] = {};
        bindings[i + 1].binding         = i + 1;
        bindings[i + 1].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i + 1].descriptorCount = 1;
        bindings[i + 1].stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    // bindings 1..4 允许 update-after-bind（draw 期间更新纹理槽位）
    VkDescriptorBindingFlags bindFlags[5]{};
    for (uint32_t i = 1; i < 5; ++i) bindFlags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;

    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount  = 5;
    bfi.pBindingFlags = bindFlags;

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings    = bindings;
    li.pNext        = &bfi;
    li.flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
    VK_CHECK(vkCreateDescriptorSetLayout(_device, &li, nullptr, &_texSetLayout));

    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 128;
    poolSizes[1].type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 32;
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
    pi.maxSets       = 32;
    pi.poolSizeCount = 2;
    pi.pPoolSizes    = poolSizes;
    VK_CHECK(vkCreateDescriptorPool(_device, &pi, nullptr, &_texDescriptorPool));
    setObjName(VK_OBJECT_TYPE_DESCRIPTOR_POOL, reinterpret_cast<uint64_t>(_texDescriptorPool), "RHI_TexDescriptorPool");
    setObjName(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT, reinterpret_cast<uint64_t>(_texSetLayout), "RHI_TexSetLayout");

    // 默认 uniform block：未显式持有 block 的绘制（冒烟三角形/贴图四边形）使用
    _defaultUniformBlock = createUniformBlock(kDrawUniformsSize);
}

void VulkanDevice::createWhiteFallbackTexture() {
    // 白色兜底纹理：未绑定的描述符槽位也指向合法资源（2D + cubemap 各一）。
    // 注意：仅创建纹理；采样器槽位由 createUniformBlock 创建块时预填
    //（白纹理必须先于描述符对象创建，见构造顺序）
    uint8_t white[4] = {255, 255, 255, 255};
    TextureDesc d;
    d.width = 1; d.height = 1;
    d.usage = TEX_SAMPLED;
    d.initialData = white;
    _whiteTex = createTexture(d);

    uint8_t cubeWhite[4 * 6] = {255,255,255,255, 255,255,255,255, 255,255,255,255,
                                255,255,255,255, 255,255,255,255, 255,255,255,255};
    TextureDesc cd;
    cd.width = 1; cd.height = 1;
    cd.usage = TEX_SAMPLED;
    cd.cubemap = true;
    cd.initialData = cubeWhite;
    _whiteCubeTex = createTexture(cd);
}

void VulkanDevice::submitOneTime(std::function<void(VkCommandBuffer)> record, std::function<void()> onComplete) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = _commandPool;
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    VK_CHECK(vkAllocateCommandBuffers(_device, &ai, &cb));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    VK_CHECK(vkBeginCommandBuffer(cb, &bi));
    record(cb);
    VK_CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    VK_CHECK(vkQueueSubmit(_queue, 1, &si, VK_NULL_HANDLE));
    VK_CHECK(vkQueueWaitIdle(_queue));
    // GPU 执行完成后才允许释放回调中引用的资源（如 staging）
    if (onComplete) onComplete();
    vkFreeCommandBuffers(_device, _commandPool, 1, &cb);
}

UniformBlockHandle VulkanDevice::createUniformBlock(uint32_t size) {
    VkUniformBlockRes blk;
    blk.size = size;

    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size  = size;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(_device, &bi, nullptr, &blk.buffer));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(_device, blk.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = findMemoryType(_physicalDevice, req.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK(vkAllocateMemory(_device, &mai, nullptr, &blk.memory));
    VK_CHECK(vkBindBufferMemory(_device, blk.buffer, blk.memory, 0));
    VK_CHECK(vkMapMemory(_device, blk.memory, 0, size, 0, &blk.mapped));

    // 独立描述符集：binding0 = UBO；bindings1..4 = 白色兜底采样器
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool     = _texDescriptorPool;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts        = &_texSetLayout;
    VK_CHECK(vkAllocateDescriptorSets(_device, &dai, &blk.set));

    VkDescriptorBufferInfo bufInfo{blk.buffer, 0, size};
    VkWriteDescriptorSet writes[5]{};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[0].dstSet = blk.set;
    writes[0].dstBinding = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &bufInfo;

    const VkTextureRes& white = _textures[_whiteTex.id - 1];
    const VkTextureRes& whiteCube = _textures[_whiteCubeTex.id - 1];
    VkDescriptorImageInfo imgInfos[4];
    for (uint32_t i = 0; i < 4; ++i) {
        const bool isCubeSlot = (i == 3);   // 槽 3 = cubemap（天空盒）
        const VkTextureRes& fill = isCubeSlot ? whiteCube : white;
        imgInfos[i] = { fill.sampler, fill.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        writes[i + 1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i + 1].dstSet = blk.set;
        writes[i + 1].dstBinding = i + 1;
        writes[i + 1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i + 1].descriptorCount = 1;
        writes[i + 1].pImageInfo = &imgInfos[i];
    }
    vkUpdateDescriptorSets(_device, 5, writes, 0, nullptr);
    char ubname[48];
    std::snprintf(ubname, sizeof(ubname), "RHI_UniformBlock_%u", static_cast<uint32_t>(_uniformBlocks.size() + 1));
    setObjName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(blk.buffer), ubname);
    setObjName(VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<uint64_t>(blk.set), ubname);

    _uniformBlocks.push_back(blk);
    UniformBlockHandle h; h.id = static_cast<uint32_t>(_uniformBlocks.size());
    return h;
}

void VulkanDevice::bindInstanceVertexBuffer(VkCommandBuffer cb, BufferHandle h) const {
    if (!h.valid() || h.id > _buffers.size()) return;
    VkBuffer vb = _buffers[h.id - 1].buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 1, 1, &vb, &offset);   // binding1 = 实例率
}

uint64_t VulkanDevice::nativeHandle(RhiHandle handle) const {
    if (handle.id >= 1 && handle.id <= _buffers.size()) {
        return reinterpret_cast<uint64_t>(_buffers[handle.id - 1].buffer);
    }
    return 0;
}

DeviceInfo VulkanDevice::info() const {
    DeviceInfo di;
    di.backend     = Backend::Vulkan;
    di.backendName = "Vulkan (MoltenVK)";
    di.adapterName = _adapterName;
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(_physicalDevice, &props);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "Vulkan %u.%u.%u",
                  VK_VERSION_MAJOR(props.apiVersion),
                  VK_VERSION_MINOR(props.apiVersion),
                  VK_VERSION_PATCH(props.apiVersion));
    di.apiVersion = buf;
    return di;
}

DeviceCapabilities VulkanDevice::capabilities() const {
    DeviceCapabilities caps;
    caps.maxTexture2D           = _caps.props.limits.maxImageDimension2D;
    caps.maxPushConstantSize    = _caps.props.limits.maxPushConstantsSize;
    caps.maxUniformBufferRange  = _caps.props.limits.maxUniformBufferRange;
    caps.maxSampledImagesPerSet = _caps.props.limits.maxDescriptorSetSampledImages;
    for (uint32_t i = 0; i < _caps.memory.memoryHeapCount; ++i) {
        const auto& h = _caps.memory.memoryHeaps[i];
        if (h.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) caps.deviceMemoryBytes += h.size;
    }
    caps.dynamicRendering   = _caps.hasDynamicRendering;
    caps.synchronization2   = _caps.hasSynchronization2;
    caps.timelineSemaphore  = _caps.hasTimelineSemaphore;
    caps.descriptorIndexing = _caps.hasDescriptorIndexing;
    caps.memoryBudget       = _caps.hasMemoryBudget;
    caps.wideLines          = _caps.features.wideLines == VK_TRUE;
    caps.geometryShader     = _caps.features.geometryShader == VK_TRUE;
    caps.samplerAnisotropy  = _caps.features.samplerAnisotropy == VK_TRUE;
    for (const auto& f : _caps.surfaceFormats) {
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB || f.format == VK_FORMAT_R8G8B8A8_SRGB) {
            caps.sRGBSwapchain = true;
            break;
        }
    }

    caps.details = _caps.toCapabilities();
    caps.details.push_back({"验证层", _validationEnabled ? "enabled" : "disabled"});
    caps.details.push_back({"debug_utils", _debugUtilsEnabled ? "enabled" : "disabled"});
    if (_pipelineCache) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s, 启动时 %zu B",
                      _pipelineCacheFromDisk ? "loaded" : "new",
                      _pipelineCacheLoadedBytes);
        caps.details.push_back({"pipeline cache", buf});
    } else {
        caps.details.push_back({"pipeline cache", "disabled"});
    }
    return caps;
}

} // namespace rhi
