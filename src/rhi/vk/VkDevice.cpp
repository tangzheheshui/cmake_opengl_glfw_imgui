//
//  VulkanDevice.cpp
//
#include "log/LogMacros.h"
#include "VkDevice.h"
#include "VkCommandList.h"
#include "rhi/FormatInfo.h"
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <filesystem>
#include "fs/FilePathUtil.h"
#include "VkShaderReflect.h"

// C1a：平台 surface 建面宏由 CMake 注入（VK_USE_PLATFORM_*）。公共头不出现
// Vk 类型；此处是全库唯一允许平台分支的位置（后端内部实现细节）。
#if defined(VK_USE_PLATFORM_WIN32_KHR)
#include <windows.h>
#include <vulkan/vulkan_win32.h>
#endif
#if defined(VK_USE_PLATFORM_METAL_EXT)
#include <vulkan/vulkan_metal.h>
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
#include <X11/Xlib.h>
#include <vulkan/vulkan_xlib.h>
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
#include <xcb/xcb.h>
#include <vulkan/vulkan_xcb.h>
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
#include <wayland-client.h>
#include <vulkan/vulkan_wayland.h>
#endif

namespace rhi {

// C1a：按 NativeWindowType 推导实例扩展（宿主未提供 instanceExtensions 时）。
// 返回 false = 该窗口类型无对应平台 surface 扩展（无法自建面）。
static bool platformInstanceExtensions(NativeWindowType t,
                                       std::vector<const char*>& out) {
    switch (t) {
        case NativeWindowType::Win32:
            out.push_back("VK_KHR_surface");
            out.push_back("VK_KHR_win32_surface");
            return true;
        case NativeWindowType::Xlib:
            out.push_back("VK_KHR_surface");
            out.push_back("VK_KHR_xlib_surface");
            return true;
        case NativeWindowType::Xcb:
            out.push_back("VK_KHR_surface");
            out.push_back("VK_KHR_xcb_surface");
            return true;
        case NativeWindowType::Wayland:
            out.push_back("VK_KHR_surface");
            out.push_back("VK_KHR_wayland_surface");
            return true;
        case NativeWindowType::MetalLayer:
        case NativeWindowType::Cocoa:   // Cocoa 窗口经 CAMetalLayer 建面（宿主回调常见）
            out.push_back("VK_KHR_surface");
            out.push_back("VK_EXT_metal_surface");
            return true;
        case NativeWindowType::None:
        default:
            return false;
    }
}

static const char* kRequiredDeviceExts[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

// ---- 工具：读取 SPIR-V 文件 ----
// A4: 路径按 UTF-8 解释（Windows 上宽字符打开，中文安装路径可用）
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
// C1a：由 DeviceDesc 构造。早期初始化失败写入 outError 并保持 _initialized=false
//（工厂转译为 nullptr + 诊断信息）；深度失败（资源路径 VK_CHECK）仍 abort，
// V3-1 引入 Result<T> 后收口。
VulkanDevice::VulkanDevice(const DeviceDesc& desc, std::string& outError)
    : _desc(desc)
    , _shaderRootPath(desc.shaderRootPath)
    , _size{desc.width, desc.height} {
    if (desc.offscreen) {
        outError = "offscreen Vulkan device not supported yet (C4/F1)";
        return;
    }
    createInstance();
    if (!createSurface(outError)) return;
    pickPhysicalDevice();
    // A3: 能力探测（启动日志打印能力表，多机验证的第一手资料）
    _caps = VkCaps::query(_physicalDevice, _surface);
    _caps.log();
    createLogicalDevice();
    _memAlloc.init(_physicalDevice, _device);   // E1：先于任何资源创建
    createSwapchainObjects();
    createCommandObjects();
    createSyncObjects();
    // A1: 加载对象命名函数指针（debug_utils 扩展启用后可用）
    _setNameFn = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
        vkGetInstanceProcAddr(_instance, "vkSetDebugUtilsObjectNameEXT"));
    // V3-2：描述符布局/池先建（纹理创建时就要分配自己的描述符集），
    // 白兜底纹理随后创建（它同样走 createTexture，拿到的 set 供未绑定槽位使用）
    createDescriptorObjects();
    // V3-3：管线创建器（含磁盘管线缓存加载）。需要描述符里的全局管线布局
    {
        VkPipelines::NameFn nameFn = [this](VkObjectType type, uint64_t handle, const char* name) {
            setObjName(type, handle, name);
        };
        _pipes.init(_device, &_caps, _shaderRootPath, _descs.pipelineLayout(), nameFn);
    }
    createWhiteFallbackTexture();
    // 白纹理创建完才知道它们的描述符集，这里交给 VkDescriptors 做兜底
    _descs.setFallbackSets(_textures.get(_whiteTex.id).set,
                           _textures.get(_whiteCubeTex.id).set);
    _commandList = std::make_unique<VkCommandList>(this);
    _initialized = true;
}

VulkanDevice::~VulkanDevice() {
    if (_device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(_device);
    }
    // E1：排空帧内延迟的 GPU 工作（如最后帧的纹理 staging）——此前退出路径
    // 不 drain，其 staging 的释放回调永远不执行（分配器统计暴露的既有泄漏）
    drainDeferredGpuWork();
    // V3-0：延迟销毁兜底必须在 _memAlloc.shutdown() 之前——shutdown 时
    // （RenderSystem::shutdown 释放 pick RT 等）刚入队的条目尚未 flush，
    // 若先跑分配器统计会误报 live>0，且回收顺序颠倒
    _delQueue.flushAll();
    destroySwapchainObjects();
    _frames.shutdown();
    if (_commandPool)       vkDestroyCommandPool(_device, _commandPool, nullptr);
    // E1：先走子分配器统计/告警，资源对象内存在随行销毁
    _buffers.forEach([&](VkBufferRes& b) {
        if (b.buffer) vkDestroyBuffer(_device, b.buffer, nullptr);
        _memAlloc.free(b.memAlloc);   // E1：内存归分配器（防双重释放）
    });
    _textures.forEach([&](VkTextureRes& t) {
        if (t.sampler) vkDestroySampler(_device, t.sampler, nullptr);
        if (t.view)    vkDestroyImageView(_device, t.view, nullptr);
        if (t.image)   vkDestroyImage(_device, t.image, nullptr);
        _memAlloc.free(t.memAlloc);   // E1
    });
    // V3-0：离屏 RT 的深度附件兜底回收（宿主未调 destroyRenderTarget 的槽位；
    // 颜色附件是 VkTextureRes，已在上方 _textures 循环销毁）
    _renderTargets.forEach([&](VkRenderTargetRes& rt) {
        if (rt.depthView)  vkDestroyImageView(_device, rt.depthView, nullptr);
        if (rt.depthImage) vkDestroyImage(_device, rt.depthImage, nullptr);
        _memAlloc.free(rt.depthMemAlloc);
    });
    // destroySwapchainObjects 只回收 swapchain/renderPass，不回收管线
    _pipelines.forEach([&](VkPipelineRes& p) {
        if (p.pipeline) vkDestroyPipeline(_device, p.pipeline, nullptr);
    });
    // V3-3：布局/池/帧 uniform 环归 VkDescriptors 回收
    _descs.shutdown();
    _pipes.saveAndDestroy();   // 导出管线缓存并销毁缓存对象（须在 destroy device 之前）
    _memAlloc.shutdown();    // E1：设备销毁前回收页/专用分配（泄漏在此告警）
    if (_device)   vkDestroyDevice(_device, nullptr);
    // C1a 所有权约定：externalSurface 归宿主；createSurface 回调/平台自建归 RHI
    if (_surface && _surfaceOwned) vkDestroySurfaceKHR(_instance, _surface, nullptr);
    if (_debugMessenger && _destroyDebugMessengerFn) {
        _destroyDebugMessengerFn(_instance, _debugMessenger, nullptr);
    }
    if (_instance) vkDestroyInstance(_instance, nullptr);
}

void VulkanDevice::createInstance() {
    // 1) 平台 surface 扩展：宿主显式提供（instanceExtensions）优先；
    //    否则按 NativeWindowType 推导（C1a，RHI 不依赖任何窗口库）
    std::vector<const char*> exts;
    if (!_desc.instanceExtensions.empty()) {
        for (const auto& e : _desc.instanceExtensions) {
            exts.push_back(e.c_str());   // 指向 _desc 成员字符串，设备生命周期内稳定
        }
        ENGINE_LOG_INFO("[VK] instance extensions provided by host (%zu)",
                        _desc.instanceExtensions.size());
    } else {
        std::vector<const char*> plat;
        if (!platformInstanceExtensions(_desc.window.type, plat)) {
            ENGINE_LOG_ERROR("[VK] no platform surface extension for window type %d "
                             "(provide DeviceDesc.createSurface or instanceExtensions)",
                             static_cast<int>(_desc.window.type));
            return;   // _initialized 保持 false
        }
        exts = plat;
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

    // A1: 验证层开关（RHI_VK_VALIDATION=1 强制开 / =0 强制关；Debug 构建默认开；
    // DeviceDesc::enableValidation=false 关闭——环境变量可覆盖宿主设置）
    const char* venv = std::getenv("RHI_VK_VALIDATION");
    bool wantValidation = _desc.enableValidation;
    if (venv) {
        wantValidation = (std::strcmp(venv, "0") != 0);
    }
#ifndef NDEBUG
    else if (!_desc.enableValidation) {
        wantValidation = false;   // 宿主显式关闭
    } else {
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

// C1a：surface 来源优先级 externalSurface（宿主持有，RHI 不销毁）
// → createSurface 回调（RHI 接管）→ 平台自建（RHI 接管）。
bool VulkanDevice::createSurface(std::string& outError) {
    if (_desc.externalSurface != 0) {
        _surface = reinterpret_cast<VkSurfaceKHR>(_desc.externalSurface);
        _surfaceOwned = false;
        ENGINE_LOG_INFO("[VK] using host-owned external surface");
        return true;
    }
    if (_desc.createSurface) {
        const uint64_t s = _desc.createSurface(reinterpret_cast<uint64_t>(_instance));
        if (s == 0) {
            outError = "host createSurface callback returned 0";
            return false;
        }
        _surface = reinterpret_cast<VkSurfaceKHR>(s);
        _surfaceOwned = true;
        return true;
    }

#if defined(VK_USE_PLATFORM_METAL_EXT)
    if (_desc.window.type == NativeWindowType::MetalLayer
        || _desc.window.type == NativeWindowType::Cocoa) {
        if (_desc.window.type == NativeWindowType::Cocoa) {
            // Cocoa 窗口句柄（NSView*）不能直接建面；需要 CAMetalLayer。
            // 由宿主提供 createSurface 回调（GLFW 宿主内部处理窗口→layer 转换）
            outError = "Cocoa window requires host createSurface callback (NSView -> CAMetalLayer)";
            return false;
        }
        VkMetalSurfaceCreateInfoEXT mi{VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT};
        mi.pLayer = _desc.window.handle;   // CAMetalLayer*
        mi.flags  = 0;
        if (vkCreateMetalSurfaceEXT(_instance, &mi, nullptr, &_surface) != VK_SUCCESS) {
            outError = "vkCreateMetalSurfaceEXT failed";
            return false;
        }
        _surfaceOwned = true;
        return true;
    }
#endif
#if defined(VK_USE_PLATFORM_WIN32_KHR)
    if (_desc.window.type == NativeWindowType::Win32) {
        VkWin32SurfaceCreateInfoKHR mi{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        mi.hinstance = GetModuleHandle(nullptr);
        mi.hwnd      = static_cast<HWND>(_desc.window.handle);
        if (vkCreateWin32SurfaceKHR(_instance, &mi, nullptr, &_surface) != VK_SUCCESS) {
            outError = "vkCreateWin32SurfaceKHR failed";
            return false;
        }
        _surfaceOwned = true;
        return true;
    }
#endif
#if defined(VK_USE_PLATFORM_XLIB_KHR)
    if (_desc.window.type == NativeWindowType::Xlib) {
        VkXlibSurfaceCreateInfoKHR mi{VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR};
        mi.dpy       = static_cast<Display*>(_desc.window.display);
        mi.window    = reinterpret_cast<Window>(_desc.window.handle);
        if (vkCreateXlibSurfaceKHR(_instance, &mi, nullptr, &_surface) != VK_SUCCESS) {
            outError = "vkCreateXlibSurfaceKHR failed";
            return false;
        }
        _surfaceOwned = true;
        return true;
    }
#endif
#if defined(VK_USE_PLATFORM_XCB_KHR)
    if (_desc.window.type == NativeWindowType::Xcb) {
        VkXcbSurfaceCreateInfoKHR mi{VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR};
        mi.connection = static_cast<xcb_connection_t*>(_desc.window.display);
        mi.window     = static_cast<xcb_window_t>(reinterpret_cast<uintptr_t>(_desc.window.handle));
        if (vkCreateXcbSurfaceKHR(_instance, &mi, nullptr, &_surface) != VK_SUCCESS) {
            outError = "vkCreateXcbSurfaceKHR failed";
            return false;
        }
        _surfaceOwned = true;
        return true;
    }
#endif
#if defined(VK_USE_PLATFORM_WAYLAND_KHR)
    if (_desc.window.type == NativeWindowType::Wayland) {
        VkWaylandSurfaceCreateInfoKHR mi{VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR};
        mi.display   = static_cast<wl_display*>(_desc.window.display);
        mi.surface   = static_cast<wl_surface*>(_desc.window.handle);
        if (vkCreateWaylandSurfaceKHR(_instance, &mi, nullptr, &_surface) != VK_SUCCESS) {
            outError = "vkCreateWaylandSurfaceKHR failed";
            return false;
        }
        _surfaceOwned = true;
        return true;
    }
#endif
    (void)outError;
    outError = "no platform surface path for window type "
               + std::to_string(static_cast<int>(_desc.window.type))
               + " (platform macro not compiled in?)";
    return false;
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
        // dynamic rendering 的依赖扩展（部分已 1.1/1.2 core 化）。1.2 设备必须启用；
        // 1.3 设备虽已 core 化，但部分验证层对 KHR 别名仍要求显式列出（VUID 01387），
        // 故统一启用。
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
        // 1.3 设备也显式启用 KHR 扩展：实例按 1.0 请求时 1.3 core 入口/特性不可用，
        // 只有启用扩展后 KHR 别名才可解析（Windows 实机暴露）。
        devExts.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
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

    // B1：dynamic rendering 特性（统一走 KHR 扩展——实例按 1.0 请求时 1.3 core
    // 特性 struct 不可用，启用扩展后走 KHR struct，1.2/1.3 路径一致）
    VkPhysicalDeviceDynamicRenderingFeaturesKHR dynFeat{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES_KHR};
    dynFeat.dynamicRendering = VK_TRUE;
    vk12.pNext = &dynFeat;        // vk12 → KHR dynamic rendering
    void* featureChain = &vk12;

    // B3：启用 samplerAnisotropy（各向异性过滤，纹理采样描述符 SamplerDesc 使用；
    // 物理设备不支持时保持关闭，createSampler 会跳过 anisotropyEnable）
    // D5：fillModeNonSolid（线框显示模式的 VK_POLYGON_MODE_LINE；MoltenVK 经
    // MTLTriangleFillModeLines 支持）
    VkPhysicalDeviceFeatures enabledFeatures{};
    enabledFeatures.samplerAnisotropy = _caps.features.samplerAnisotropy;
    enabledFeatures.fillModeNonSolid  = _caps.features.fillModeNonSolid;

    VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    ci.queueCreateInfoCount    = 1;
    ci.pQueueCreateInfos       = &qci;
    ci.enabledExtensionCount   = static_cast<uint32_t>(devExts.size());
    ci.ppEnabledExtensionNames = devExts.data();
    ci.pEnabledFeatures        = &enabledFeatures;
    ci.pNext                   = featureChain;
    VK_CHECK(vkCreateDevice(_physicalDevice, &ci, nullptr, &_device));
    vkGetDeviceQueue(_device, _queueFamily, 0, &_queue);
    // B1：动态渲染入口点（KHR 扩展已统一启用，走 KHR 别名）
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
    // V3-3：交换链/深度/信号量都交给 VkSwapchain（见 VkSwapchain.cpp）
    VkSwapchain::NameFn nameFn = [this](VkObjectType type, uint64_t handle, const char* name) {
        setObjName(type, handle, name);
    };
    if (!_swapchain.create(_physicalDevice, _device, _surface, _size.width, _size.height,
                           findSupportedDepthFormat(_physicalDevice), &_memAlloc, nameFn)) {
        ENGINE_LOG_ERROR("[VK] swapchain creation failed");
    }
}

void VulkanDevice::destroySwapchainObjects() {
    _swapchain.destroy();
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
    // 命令缓冲按帧槽位分配（见 VkFrames::init）
    setObjName(VK_OBJECT_TYPE_COMMAND_POOL, reinterpret_cast<uint64_t>(_commandPool), "RHI_CommandPool");
}

void VulkanDevice::createSyncObjects() {
    // V3-4：帧在飞 = 2。每槽位一套 命令缓冲 + fence + image-available 信号量
    VkFrames::NameFn nameFn = [this](VkObjectType type, uint64_t handle, const char* name) {
        setObjName(type, handle, name);
    };
    if (!_frames.init(_device, _commandPool, VkFrames::kDefaultSlots, nameFn)) {
        ENGINE_LOG_ERROR("[VK] frame sync objects init failed");
    }
}

// ============================================================
// A3: Pipeline Cache 持久化
// ============================================================
// 缓存文件头校验：跨驱动/跨设备的缓存可能不兼容。规范要求驱动忽略不兼容数据，
// 但主动校验可避免验证层告警，也能在日志里说明"为什么没加速"。
// ============================================================
// 帧循环
// ============================================================
bool VulkanDevice::beginFrameInternal() {
    // V3-4：等本槽位上一次提交完成（首帧 fence 预置已信号，直接通过）
    if (!_frames.wait()) {
        ENGINE_LOG_ERROR("[VK] frame fence wait failed");
        return false;
    }
    if (!_swapchain.acquire(_frames.current().imageAvailable, _imageIndex)) {
        return false;   // OUT_OF_DATE 等：本帧跳过，beginFrame 下一轮重建
    }
    if (!_frames.beginRecording()) {
        ENGINE_LOG_ERROR("[VK] frame command buffer begin failed");
        return false;
    }
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

bool VulkanDevice::beginFrame() {
    drainDeferredGpuWork();   // 上一帧帧内延迟的纹理上传：现在帧外安全执行
    if (_swapchain.needsRecreate()) {
        vkDeviceWaitIdle(_device);
        destroySwapchainObjects();
        createSwapchainObjects();
        // B1：dynamic rendering 下管线不引用 renderPass/framebuffer，
        // swapchain 重建不再需要重建 PSO（原来 recreatePipelines 的唯一理由）
    }
    _frameSkipped = !beginFrameInternal();
    _frameActive  = !_frameSkipped;
    if (_frameActive) {
        // V3-0：fence 已等待 = 上一帧提交完成，此处的到期销毁安全。
        // 帧号先 flush 再递增（登记 base = 已完成的最近帧）。
        _delQueue.flush(_frameCount);
        ++_frameCount;

        // V3-2：帧内 uniform 环归零。上一帧命令已执行完（fence 已等待），
        // 现在整块复用是安全的；offset 0 留给"没调 setDrawUniforms 的绘制"。
        _descs.beginFrame(_frames.index());

        if (_commandList) _commandList->onFrameBegin();
    }
    return _frameActive;
}

void VulkanDevice::endFrame() {
    if (!_frameActive) { _frameSkipped = false; return; }
    _frameActive = false;

    // B1：dynamic rendering 无 renderpass finalLayout，present 前手动转回 PRESENT_SRC。
    // 仅在本帧真正渲染过（布局已被转到 COLOR_ATTACHMENT）时需要；空帧已是 PRESENT。
    VkCommandBuffer cb = _frames.current().cmd;
    recordPresentBarrier(cb);
    VK_CHECK(vkEndCommandBuffer(cb));

    // 每个 swapchain image 使用独立的 render-done semaphore：
    // presentation 完成前不能复用同一个 binary semaphore（VUID-...-00067）
    VkSemaphore renderDone = _swapchain.renderDoneSemaphore(_imageIndex);
    if (renderDone == VK_NULL_HANDLE) {
        ENGINE_LOG_ERROR("[VK] missing render-done semaphore for image %u", _imageIndex);
        return;
    }

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    VkSemaphore imageAvailable = _frames.current().imageAvailable;
    si.pWaitSemaphores    = &imageAvailable;
    si.pWaitDstStageMask  = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &renderDone;
    VK_CHECK(vkQueueSubmit(_queue, 1, &si, _frames.current().fence));
    _frameSubmitted = true;   // C1b：present() 消费该标志
    _frames.advance();        // 下一帧换槽位（本帧 GPU 未完成前不覆盖它）

    // ---- RHI_PROBE=1：第 30 帧回读中心像素（M3 验证用，之后可移除）----
    // 必须在 present 之前执行：此时图像仍处于 acquired 状态，布局转换合法。
    // 单帧调试路径，允许同步 vkQueueWaitIdle。
    static int frameCount = 0;
    static bool probeEnabled = (std::getenv("RHI_PROBE") != nullptr);
    if (probeEnabled && ++frameCount == 30) {
        vkQueueWaitIdle(_queue);   // 主提交完成；图像处于 PRESENT_SRC，仍 acquired
        VkBuffer readback;
        VkMemAlloc readbackMem;   // E1：子分配 + 页持久映射
        const VkExtent2D scExtent = _swapchain.extent();
        VkDeviceSize bufSize = scExtent.width * scExtent.height * 4;
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size  = bufSize;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VK_CHECK(vkCreateBuffer(_device, &bci, nullptr, &readback));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(_device, readback, &req);
        readbackMem = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                          | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_CHECK(vkBindBufferMemory(_device, readback, readbackMem.memory, readbackMem.offset));

        // 主命令缓冲已完成，可 reset 后复用录制回读
        VkCommandBuffer cb = _frames.current().cmd;
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
        toTransfer.image = _swapchain.image(_imageIndex);
        toTransfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toTransfer);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {scExtent.width, scExtent.height, 1};
        vkCmdCopyImageToBuffer(cb, _swapchain.image(_imageIndex),
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &region);
        // 拷贝后必须恢复 PRESENT_SRC，才能交给 present
        VkImageMemoryBarrier toPresent{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toPresent.dstAccessMask = 0;
        toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        toPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toPresent.image = _swapchain.image(_imageIndex);
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

        // E1：页持久映射，直接取指针
        const uint8_t* px = static_cast<const uint8_t*>(readbackMem.mapped);
        uint32_t cx = scExtent.width / 2, cy = scExtent.height * 2 / 5;
        const size_t off = (static_cast<size_t>(cy) * scExtent.width + cx) * 4;
        // swapchain 格式可能是 BGRA：打印原始字节 + 两种解释
        ENGINE_LOG_INFO("[VKPROBE] center bytes = %d,%d,%d,%d (format=%d)",
                    px[off], px[off+1], px[off+2], px[off+3], (int)_swapchain.format());
        std::fflush(stdout);
        if (std::getenv("RHI_DUMP")) {   // 正式调试设施：导出整帧 PPM（跨平台临时目录）
            std::error_code ec;
            auto tmpDir = std::filesystem::temp_directory_path(ec);
            if (ec) tmpDir = ".";
            // A4: 保持 path 对象直接打开（临时目录可能含中文用户名，Windows ACP fopen 会失败）
            FILE* fp = engine::fs::openWrite(tmpDir / "frame_vk.ppm");
            if (fp) {
                std::fprintf(fp, "P6\n%u %u\n255\n", scExtent.width, scExtent.height);
                // B3a 完成后 3D 内容已是正立，按屏幕行序直接导出
                for (uint32_t y = 0; y < scExtent.height; ++y) {
                    const uint8_t* row = px + static_cast<size_t>(y) * scExtent.width * 4;
                    for (uint32_t x = 0; x < scExtent.width; ++x) {
                        const uint8_t* px4 = row + static_cast<size_t>(x) * 4;
                        const uint8_t rgb[3] = { px4[2], px4[1], px4[0] };   // BGRA → RGB 真色
                        fwrite(rgb, 1, 3, fp);
                    }
                }
                fclose(fp);
            }
        }
        vkDestroyBuffer(_device, readback, nullptr);
        _memAlloc.free(readbackMem);   // E1
        std::fflush(stdout);
    }
    // C1b：present 由 IRHIDevice::present() 独立执行（GL 宿主 swapBuffers 与
    // VK vkQueuePresentKHR 同一入口），endFrame 只负责 submit
}

// ---- C1a/C1b：呈现独立于提交（GL 宿主 swap 与 VK present 同一入口）----
void VulkanDevice::present() {
    if (!_frameSubmitted) return;   // 帧跳过/未提交：无可呈现图像
    _frameSubmitted = false;
    VkSemaphore renderDone = _swapchain.renderDoneSemaphore(_imageIndex);
    if (!renderDone) return;
    _swapchain.present(_queue, _imageIndex, renderDone);
}

void VulkanDevice::onResize(int width, int height) {
    // C1a：尺寸由宿主推送，保存为帧缓冲尺寸真相源（framebufferSize 返回值）
    if (width > 0 && height > 0) {
        _size.width  = static_cast<uint32_t>(width);
        _size.height = static_cast<uint32_t>(height);
    }
    // 尺寸未变则忽略（应用启动时 onWindowSizeChanged 传来的尺寸与创建时一致，
    // 若此时重建 swapchain，会销毁管线引用的 renderPass 导致绘制崩溃）
    if (width > 0 && height > 0 &&
        (static_cast<uint32_t>(width)  != _swapchain.extent().width ||
         static_cast<uint32_t>(height) != _swapchain.extent().height)) {
        _swapchain.requestRecreate();
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
    VK_CHECK_RET(vkCreateBuffer(_device, &bci, nullptr, &res.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(_device, res.buffer, &req);
    // E1：HOST_VISIBLE 子分配（页持久映射，mapped 直接可得，省 per-object map）
    res.memAlloc = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!res.memAlloc.valid()) {
        ENGINE_LOG_ERROR("[VK] buffer memory alloc failed (%u bytes)", desc.size);
        vkDestroyBuffer(_device, res.buffer, nullptr);
        return {};
    }
    res.memory = res.memAlloc.memory;
    res.mapped = res.memAlloc.mapped;
    if (vkBindBufferMemory(_device, res.buffer, res.memory, res.memAlloc.offset) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] vkBindBufferMemory failed at %s:%d", __FILE__, __LINE__);
        vkDestroyBuffer(_device, res.buffer, nullptr);
        _memAlloc.free(res.memAlloc);
        return {};
    }
    if (desc.initialData && desc.size > 0) {
        std::memcpy(res.mapped, desc.initialData, desc.size);
    }

    RhiHandle base = _buffers.alloc(std::move(res));
    BufferHandle h; h.id = base.id; h.gen = base.gen;
    char name[48];
    std::snprintf(name, sizeof(name), "RHI_Buffer_%u", h.id);
    setObjName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(res.buffer), name);
    return h;
}

void VulkanDevice::updateBuffer(BufferHandle buffer, const void* data,
                            uint32_t size, uint32_t offset) {
    if (!_buffers.alive(buffer) || !data) return;
    VkBufferRes& b = _buffers.get(buffer.id);
    if (!b.mapped || offset + size > b.size) return;   // 已销毁 / 越界保护
    std::memcpy(static_cast<uint8_t*>(b.mapped) + offset, data, size);
}

// ---- v2 对象级销毁（B0）----
// V3-0 DeletionQueue：摘句柄立即生效（槽位清空、id 不复用）；真销毁登记到
// _frameCount + kLagFrames，beginFrame 的 fence 等待之后批量执行——资源不再
// 被帧在飞的命令引用。析构路径 flushAll 兜底。
void VulkanDevice::destroyBuffer(BufferHandle h) {
    if (!_buffers.alive(h)) return;
    VkBufferRes& b = _buffers.get(h.id);
    if (!b.buffer) return;
    VkBuffer buf = b.buffer;
    VkMemAlloc mem = b.memAlloc;
    _buffers.free(h.id);
    _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                   [this, buf, mem] {
                       vkDestroyBuffer(_device, buf, nullptr);
                       _memAlloc.free(mem);
                   });
}

void VulkanDevice::destroyTexture(TextureHandle h) {
    if (!_textures.alive(h)) return;
    VkTextureRes& t = _textures.get(h.id);
    if (!t.image) return;
    VkImage img = t.image;
    VkImageView view = t.view;
    VkSampler sm = t.sampler;
    VkMemAlloc mem = t.memAlloc;
    _textures.free(h.id);
    _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                   [this, img, view, sm, mem] {
                       if (sm)   vkDestroySampler(_device, sm, nullptr);
                       if (view) vkDestroyImageView(_device, view, nullptr);
                       vkDestroyImage(_device, img, nullptr);
                       _memAlloc.free(mem);
                   });
}

void VulkanDevice::destroyPipeline(PipelineHandle h) {
    if (!_pipelines.alive(h)) return;
    VkPipelineRes& p = _pipelines.get(h.id);
    if (p.pipeline) {
        VkPipeline pipe = p.pipeline;
        // V3-0：管线可能被帧在飞的命令引用，同样延迟销毁
        _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                       [this, pipe] { vkDestroyPipeline(_device, pipe, nullptr); });
    }
    _pipelines.free(h.id);   // 标记 dead + gen++（swapchain 重建不复活）
}

void VulkanDevice::destroyUniformBlock(UniformBlockHandle h) {
    if (!_uniformBlocks.alive(h)) return;
    if (h.id == _defaultUniformBlock.id) {
        ENGINE_LOG_WARN_ONCE("[VK] destroyUniformBlock: device default block is device-owned, ignored");
        return;
    }
    // V3-2：block 没有 GPU 资源，释放槽位即可
    _uniformBlocks.free(h.id);
}

// ---- B4：离屏渲染目标 ----
RenderTargetHandle VulkanDevice::createRenderTarget(const RenderTargetDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        ENGINE_LOG_ERROR("[VK] createRenderTarget: zero-sized target");
        return {};
    }
    VkRenderTargetRes rt;
    rt.width = desc.width;
    rt.height = desc.height;

    // 颜色附件走纹理机制：采样（bindTexture/描述符）、附件、回读（TRANSFER_SRC）一体
    TextureDesc td;
    td.width  = desc.width;
    td.height = desc.height;
    td.format = desc.colorFormat;
    td.usage  = TEX_SAMPLED | TEX_COLOR_ATTACHMENT;
    td.mipLevels = desc.mipLevels;
    td.sampler.wrapU = td.sampler.wrapV = td.sampler.wrapW = WrapMode::ClampToEdge;
    rt.colorTex = createTexture(td);
    if (!rt.colorTex.valid()) {
        return {};
    }

    // 深度附件：独立 image/view（storeOp=DONT_CARE，跨 Pass 不保内容）
    if (desc.hasDepth) {
        VkFormat depthFmt = VK_FORMAT_UNDEFINED;
        switch (desc.depthFormat) {
            case Format::Depth32F: depthFmt = VK_FORMAT_D32_SFLOAT; break;
            case Format::D24S8:    depthFmt = VK_FORMAT_D24_UNORM_S8_UINT; break;
            default: depthFmt = findSupportedDepthFormat(_physicalDevice); break;
        }
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = depthFmt;
        ici.extent        = {desc.width, desc.height, 1};
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(_device, &ici, nullptr, &rt.depthImage) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] RT depth vkCreateImage failed");
            destroyTexture(rt.colorTex);
            return {};
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(_device, rt.depthImage, &req);
        rt.depthMemAlloc = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);   // E1
        if (!rt.depthMemAlloc.valid()) {
            ENGINE_LOG_ERROR("[VK] RT depth memory alloc failed");
            vkDestroyImage(_device, rt.depthImage, nullptr);
            destroyTexture(rt.colorTex);
            return {};
        }
        rt.depthMemory = rt.depthMemAlloc.memory;
        if (vkBindImageMemory(_device, rt.depthImage, rt.depthMemory, rt.depthMemAlloc.offset) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] RT depth vkBindImageMemory failed at %s:%d", __FILE__, __LINE__);
            vkDestroyImage(_device, rt.depthImage, nullptr);
            _memAlloc.free(rt.depthMemAlloc);
            destroyTexture(rt.colorTex);
            return {};
        }

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image    = rt.depthImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = depthFmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(_device, &vci, nullptr, &rt.depthView) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] RT depth vkCreateImageView failed at %s:%d", __FILE__, __LINE__);
            vkDestroyImage(_device, rt.depthImage, nullptr);
            _memAlloc.free(rt.depthMemAlloc);
            destroyTexture(rt.colorTex);
            return {};
        }
    }

    RhiHandle base = _renderTargets.alloc(std::move(rt));
    RenderTargetHandle h; h.id = base.id; h.gen = base.gen;
    char name[48];
    std::snprintf(name, sizeof(name), "RHI_RenderTarget_%u", h.id);
    const VkTextureRes& ct = rt.colorTex.valid() ? textureRes(rt.colorTex) : VkTextureRes{};
    if (ct.image) {
        setObjName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(ct.image), name);
    }
    if (rt.depthImage) {
        setObjName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(rt.depthImage), name);
    }
    ENGINE_LOG_INFO("[VK] render target %ux%u created (id=%u, depth=%d)",
                    desc.width, desc.height, h.id, desc.hasDepth ? 1 : 0);
    return h;
}

void VulkanDevice::destroyRenderTarget(RenderTargetHandle h) {
    if (!_renderTargets.alive(h)) return;
    VkRenderTargetRes& rt = _renderTargets.get(h.id);
    destroyTexture(rt.colorTex);   // V3-0：颜色附件走纹理延迟销毁
    if (rt.depthView || rt.depthImage) {
        VkImageView view = rt.depthView;
        VkImage img = rt.depthImage;
        VkMemAlloc mem = rt.depthMemAlloc;
        _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                       [this, view, img, mem] {
                           if (view) vkDestroyImageView(_device, view, nullptr);
                           vkDestroyImage(_device, img, nullptr);
                           _memAlloc.free(mem);
                       });
    }
    _renderTargets.free(h.id);
}

TextureHandle VulkanDevice::renderTargetColorTexture(RenderTargetHandle h) const {
    if (!_renderTargets.alive(h)) return {};
    return _renderTargets.get(h.id).colorTex;
}

// ---- B4：同步回读（契约 ReadbackDesc；submitOneTime 等待完成）----
bool VulkanDevice::readbackTexture(TextureHandle tex, void* outData, size_t outSize,
                                   const ReadbackDesc& rb) {
    if (!_textures.alive(tex) || !outData) return false;
    const VkTextureRes& t = _textures.get(tex.id);
    if (!t.image || t.bpp == 0) return false;

    // 区域：宽高 0 = 整级（mip 级尺寸按 2^level 收缩）
    const uint32_t mipW = std::max(1u, t.width  >> rb.mipLevel);
    const uint32_t mipH = std::max(1u, t.height >> rb.mipLevel);
    const uint32_t w = rb.width  ? std::min(rb.width,  mipW) : mipW;
    const uint32_t h = rb.height ? std::min(rb.height, mipH) : mipH;
    const size_t bytes = static_cast<size_t>(w) * h * t.bpp;
    if (outSize < bytes) {
        ENGINE_LOG_WARN("[VK] readback: outSize %zu < %zu bytes", outSize, bytes);
        return false;
    }

    // 回读缓冲（HOST_VISIBLE，行距 0 = 紧凑；E1 子分配，页持久映射免 map）
    VkBuffer dst;
    VkMemAlloc dstMem;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(_device, &bci, nullptr, &dst));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(_device, dst, &req);
    dstMem = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                 | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!dstMem.valid()) {
        ENGINE_LOG_ERROR("[VK] readback staging alloc failed");
        vkDestroyBuffer(_device, dst, nullptr);
        return false;
    }
    if (vkBindBufferMemory(_device, dst, dstMem.memory, dstMem.offset) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] readback vkBindBufferMemory failed at %s:%d", __FILE__, __LINE__);
        vkDestroyBuffer(_device, dst, nullptr);
        _memAlloc.free(dstMem);
        return false;
    }

    VkImageLayout curLayout = t.layout;   // lambda 内访问布局快照
    const VkImage image = t.image;
    submitOneTime([this, image, curLayout, dst, w, h, mip = rb.mipLevel, layer = rb.arrayLayer, bytes](VkCommandBuffer cb) {
        // 当前布局（SHADER_READ_ONLY/UNDEFINED）→ TRANSFER_SRC
        VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.oldLayout = curLayout;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = image;
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toSrc);
        VkBufferImageCopy region{};
        region.bufferRowLength = 0;   // 紧凑存放
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
        // 回 SHADER_READ_ONLY（采样正常可用）
        VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = image;
        toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toRead);
    }, [this, dst, dstMem, outData, bytes]() {
        // GPU 完成后回读（submitOneTime 的完成回调；E1：页持久映射直接拷）
        if (dstMem.mapped) {
            std::memcpy(outData, dstMem.mapped, bytes);
        }
        vkDestroyBuffer(_device, dst, nullptr);
        _memAlloc.free(dstMem);
    });
    // 布局跟踪回写（submitOneTime 已同步完成）
    _textures.get(tex.id).layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

// ---- B4：RT 布局屏障（命令列表在 beginRenderPass/endRenderPass 的 RT 分支调用）----
void VulkanDevice::recordRenderTargetBeginBarriers(VkCommandBuffer cb, RenderTargetHandle rt) {
    VkRenderTargetRes& r = _renderTargets.get(rt.id);
    if (r.colorTex.valid()) {
        VkTextureRes& t = _textures.get(r.colorTex.id);
        if (t.image && t.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            bar.srcAccessMask = (t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                    ? VK_ACCESS_SHADER_READ_BIT : 0;
            bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            bar.oldLayout = t.layout;
            bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = t.image;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cb,
                                 t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &bar);
            t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }
    if (r.depthImage && r.depthLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        // 首次使用：UNDEFINED → DEPTH_STENCIL_ATTACHMENT（loadOp 恒 CLEAR，内容本就丢弃）。
        // 注意用 1.0 时代的 DEPTH_STENCIL_ATTACHMENT_OPTIMAL：DEPTH_ATTACHMENT_OPTIMAL
        // 是 1.2 布局，需要 separate_depth_stencil_layouts 扩展（实例按 1.0 请求时不可用）
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bar.oldLayout = r.depthLayout;
        bar.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = r.depthImage;
        bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &bar);
        r.depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
}

void VulkanDevice::recordRenderTargetEndBarriers(VkCommandBuffer cb, RenderTargetHandle rt) {
    VkRenderTargetRes& r = _renderTargets.get(rt.id);
    if (!r.colorTex.valid()) return;
    VkTextureRes& t = _textures.get(r.colorTex.id);
    if (!t.image || t.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) return;
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = t.image;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // dstAccess 含 TRANSFER_READ（回读就绪），stage 掩码必须同时覆盖 TRANSFER
    //（验证层：dstAccessMask 不受 dstStageMask 支持即报 error）
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                         | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &bar);
    t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

TextureHandle VulkanDevice::createTexture(const TextureDesc& desc) {
    VkTextureRes t;
    t.width = desc.width; t.height = desc.height;
    const uint32_t layerCount = desc.cubemap ? 6u : 1u;
    const FormatInfo& fi = FormatInfo::of(desc.format);
    const VkFormat vkFmt = static_cast<VkFormat>(fi.vkFormat());
    const uint32_t bpp = fi.bytesPerPixel();
    if (bpp == 0) {
        ENGINE_LOG_ERROR("[VK] unsupported format bpp (%d)", static_cast<int>(desc.format));
        return {};
    }
    // mip 级数：mipLevels==0 = 完整链；generateMips=true 且未显式给多级时
    // 也按完整链分配（B3c：此前 generateMips 单独使用时只分配 1 级，天空盒
    // 的 mip 链实际没有生成）
    uint32_t mipLevels = desc.mipLevels > 0 ? desc.mipLevels : 1;
    bool generateMips = desc.generateMips && desc.initialData;
    if (generateMips && mipLevels <= 1) {
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
    // B4：TRANSFER_SRC 恒开（readbackTexture 回读）；COLOR_ATTACHMENT 按 desc
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                      | (desc.initialData ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0)
                      | (generateMips ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0)
                      | ((desc.usage & TEX_COLOR_ATTACHMENT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK_RET(vkCreateImage(_device, &ici, nullptr, &t.image));
    t.bpp = bpp;   // B4：readback 区域字节数计算用

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(_device, t.image, &req);
    t.memAlloc = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);   // E1
    if (!t.memAlloc.valid()) {
        ENGINE_LOG_ERROR("[VK] texture memory alloc failed (%ux%u)", desc.width, desc.height);
        vkDestroyImage(_device, t.image, nullptr);
        return {};
    }
    t.memory = t.memAlloc.memory;
    if (vkBindImageMemory(_device, t.image, t.memory, t.memAlloc.offset) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] vkBindImageMemory failed at %s:%d", __FILE__, __LINE__);
        vkDestroyImage(_device, t.image, nullptr);
        _memAlloc.free(t.memAlloc);
        return {};
    }

    // ---- 数据上传：staging → 拷贝 + （可选）mipmap blit + 布局转换 ----
    // 可能在 render pass 活跃的帧内被调用：GPU 工作经 deferGpuWork 延迟到下一帧
    // 开头执行；数据由 lambda 持有（调用方的 initialData 生命周期未知）
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(desc.width) * desc.height * bpp * layerCount;
    // V3-2：记录上传是不是延迟到下一帧。延迟时布局在上传 lambda 里回写；
    // 立即执行（帧外创建，submitOneTime 已等待完成）时下面 alloc 之后直接置位——
    // 上传 lambda 里的回写在"立即"路径下看不到槽位（alloc 尚未发生），
    // 早先靠描述符不读布局掩盖着，接入绑定层后必须显式区分。
    const bool uploadDeferred = _frameActive;
    if (desc.initialData && bytes > 0) {
        auto dataCopy = std::make_shared<std::vector<unsigned char>>(
            static_cast<const unsigned char*>(desc.initialData),
            static_cast<const unsigned char*>(desc.initialData) + static_cast<size_t>(bytes));
        // staging 在宿主阶段创建并填充（宿主操作可安全执行于帧内）；
        // 拷贝命令延迟录制；staging 的销毁在 GPU 执行完成后由完成回调执行。
        // E1：HOST_VISIBLE 子分配（页持久映射，免 map/unmap）
        VkBuffer staging;
        VkMemAlloc stagingMem;
        VkBufferCreateInfo sbi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        sbi.size  = bytes;
        sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        sbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(_device, &sbi, nullptr, &staging) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] staging vkCreateBuffer failed (%llu bytes)",
                             static_cast<unsigned long long>(bytes));
            vkDestroyImage(_device, t.image, nullptr);
            _memAlloc.free(t.memAlloc);
            return {};
        }
        VkMemoryRequirements req2;
        vkGetBufferMemoryRequirements(_device, staging, &req2);
        stagingMem = _memAlloc.alloc(req2, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!stagingMem.valid()) {
            ENGINE_LOG_ERROR("[VK] staging alloc failed (%llu bytes)",
                             static_cast<unsigned long long>(bytes));
            vkDestroyBuffer(_device, staging, nullptr);
            vkDestroyImage(_device, t.image, nullptr);
            _memAlloc.free(t.memAlloc);
            return {};
        }
        if (vkBindBufferMemory(_device, staging, stagingMem.memory, stagingMem.offset) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] staging vkBindBufferMemory failed at %s:%d", __FILE__, __LINE__);
            vkDestroyBuffer(_device, staging, nullptr);
            _memAlloc.free(stagingMem);
            vkDestroyImage(_device, t.image, nullptr);
            _memAlloc.free(t.memAlloc);
            return {};
        }
        void* mapped = stagingMem.mapped;
        std::memcpy(mapped, dataCopy->data(), static_cast<size_t>(bytes));

        deferGpuWork([this, t, staging, stagingMem, w = desc.width, h = desc.height,
                      bpp, layerCount, mipLevels, generateMips,
                      newTexId = static_cast<uint32_t>(_textures.size()) + 1](VkCommandBuffer cb) {

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

                        // 目标级此前从未使用（UNDEFINED），先转为 TRANSFER_DST。
                        // B3c：mip 链此前因 mipLevels 恒为 1 从未真正生成，
                        // 该缺失屏障未暴露；启用完整链后被验证层捕获。
                        VkImageMemoryBarrier dstBar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        dstBar.srcAccessMask = 0;
                        dstBar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        dstBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        dstBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        dstBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        dstBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        dstBar.image = t.image;
                        dstBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i, 1, layer, 1};
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                             0, nullptr, 0, nullptr, 1, &dstBar);

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
            // B4：布局跟踪（上传完成的稳定布局；附件/回读转换以此为基）
            if (newTexId <= _textures.size()) {
                _textures.get(newTexId).layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }, [this, staging, stagingMem]() {
            vkDestroyBuffer(_device, staging, nullptr);
            _memAlloc.free(stagingMem);   // E1
        });
    }
    // ---- view + sampler ----
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image    = t.image;
    vci.viewType = desc.cubemap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = vkFmt;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels, 0, layerCount};
    if (vkCreateImageView(_device, &vci, nullptr, &t.view) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] vkCreateImageView failed at %s:%d", __FILE__, __LINE__);
        vkDestroyImage(_device, t.image, nullptr);
        _memAlloc.free(t.memAlloc);
        return {};
    }

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
    if (vkCreateSampler(_device, &sci, nullptr, &t.sampler) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] vkCreateSampler failed at %s:%d", __FILE__, __LINE__);
        vkDestroyImageView(_device, t.view, nullptr);
        vkDestroyImage(_device, t.image, nullptr);
        _memAlloc.free(t.memAlloc);
        return {};
    }

    // V3-2：纹理自带描述符集（写一次）。绘制期只绑 set，不再 vkUpdateDescriptorSets。
    {
        VkDescriptorSet texSet = VK_NULL_HANDLE;
        if (!_descs.createTextureSet(t.sampler, t.view, texSet)) {
            vkDestroySampler(_device, t.sampler, nullptr);
            vkDestroyImageView(_device, t.view, nullptr);
            vkDestroyImage(_device, t.image, nullptr);
            _memAlloc.free(t.memAlloc);
            return {};
        }
        t.set = texSet;
    }

    RhiHandle base = _textures.alloc(std::move(t));
    TextureHandle h; h.id = base.id; h.gen = base.gen;
    // 立即上传的纹理此时已处于 SHADER_READ_ONLY（见 uploadDeferred 注释）
    if (desc.initialData && bytes > 0 && !uploadDeferred) {
        _textures.get(h.id).layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    char tname[48];
    std::snprintf(tname, sizeof(tname), "RHI_Texture_%u", h.id);
    setObjName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(t.image), tname);
    setObjName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(t.view), tname);
    setObjName(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(t.sampler), tname);
    return h;
}

// ============================================================
// 查询 / 其他
// ============================================================
VkImageView VulkanDevice::currentColorView() const {
    return _swapchain.colorView(_imageIndex);
}

// B1：beginRenderPass 前的附件布局屏障。
// color：PRESENT_SRC（或首帧 UNDEFINED）→ COLOR_ATTACHMENT_OPTIMAL；
// depth：UNDEFINED（首帧，loadOp CLEAR 丢弃旧内容）→ DEPTH_ATTACHMENT_OPTIMAL，
// 之后保持不变，每帧补一条同布局的访问链屏障（前帧写 → 本帧读写）。
void VulkanDevice::recordBeginPassBarriers(VkCommandBuffer cb, bool clearColor, bool clearDepth) {
    _swapchain.recordBeginPassBarriers(cb, _imageIndex, clearColor, clearDepth);
}


// B1：present 前把当前 image 转回 PRESENT_SRC（仅当本帧渲染过）。
// V3-0：本帧未渲染（acquire 后跳过，layout 仍 UNDEFINED，如 CHURN 压测的
// 帧外 beginFrame/endFrame 循环）也必须转换——present 要求 PRESENT_SRC。
void VulkanDevice::recordPresentBarrier(VkCommandBuffer cb) {
    _swapchain.recordPresentBarrier(cb, _imageIndex);
}

bool VulkanDevice::nativeHandles(NativeHandles& out) const {
    out.instance       = reinterpret_cast<uint64_t>(_instance);
    out.physicalDevice = reinterpret_cast<uint64_t>(_physicalDevice);
    out.device         = reinterpret_cast<uint64_t>(_device);
    out.queue          = reinterpret_cast<uint64_t>(_queue);
    out.renderPass     = 0;   // B1：dynamic rendering，无 renderpass（ImGui 走 UseDynamicRendering）
    out.queueFamily    = _queueFamily;
    out.minImageCount  = 2;
    out.imageCount     = _swapchain.imageCount();
    out.colorFormat    = static_cast<uint32_t>(_swapchain.format());
    out.depthFormat    = static_cast<uint32_t>(_swapchain.depthFormat());
    return true;
}

IRHICommandList* VulkanDevice::commandList() {
    return _commandList.get();
}

// V3-3：管线创建/缓存交给 VkPipelines；设备只保留句柄表
PipelineHandle VulkanDevice::createPipeline(const GraphicsPipelineDesc& desc) {
    VkPipelineRes res;
    if (!_pipes.create(desc, _swapchain.format(), _swapchain.depthFormat(), res)) {
        return {};
    }
    RhiHandle base = _pipelines.alloc(std::move(res));
    PipelineHandle h; h.id = base.id; h.gen = base.gen;
    return h;
}

// V3-3：描述符/绑定资源统一交给 VkDescriptors（布局、池、帧 uniform 环）
void VulkanDevice::createDescriptorObjects() {
    const uint32_t minAlign = static_cast<uint32_t>(
        std::max<VkDeviceSize>(256, _caps.props.limits.minUniformBufferOffsetAlignment));
    VkDescriptors::NameFn nameFn = [this](VkObjectType type, uint64_t handle, const char* name) {
        setObjName(type, handle, name);
    };
    if (!_descs.init(_device, minAlign, &_memAlloc, VkFrames::kDefaultSlots, nameFn)) {
        ENGINE_LOG_ERROR("[VK] descriptor objects init failed");
    }
    // 默认 uniform block：未显式持有 block 的绘制（冒烟三角形/贴图四边形）使用。
    // V3-2 起只是身份与尺寸记录，没有 GPU 资源。
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
    // V3-2：不再分配 UBO/描述符集。数据统一进帧内环（见 allocDrawUniforms），
    // 句柄只保留"这是一次绘制数据块"的身份和尺寸，业务侧调用方式不变。
    VkUniformBlockRes blk;
    blk.size = size;
    RhiHandle base = _uniformBlocks.alloc(blk);
    UniformBlockHandle h; h.id = base.id; h.gen = base.gen;
    return h;
}

void VulkanDevice::bindInstanceVertexBuffer(VkCommandBuffer cb, BufferHandle h) const {
    if (!_buffers.alive(h)) return;
    VkBuffer vb = _buffers.get(h.id).buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 1, 1, &vb, &offset);   // binding1 = 实例率
}

uint64_t VulkanDevice::nativeBuffer(BufferHandle h) const {
    return _buffers.alive(h) ? reinterpret_cast<uint64_t>(_buffers.get(h.id).buffer) : 0;
}
uint64_t VulkanDevice::nativeTexture(TextureHandle h) const {
    return _textures.alive(h) ? reinterpret_cast<uint64_t>(_textures.get(h.id).image) : 0;
}
uint64_t VulkanDevice::nativePipeline(PipelineHandle h) const {
    return _pipelines.alive(h) ? reinterpret_cast<uint64_t>(_pipelines.get(h.id).pipeline) : 0;
}

// V3-1：句柄有效性（id 范围 + gen 代际校验）
bool VulkanDevice::alive(BufferHandle h)   const { return _buffers.alive(h); }
bool VulkanDevice::alive(TextureHandle h)  const { return _textures.alive(h); }
bool VulkanDevice::alive(PipelineHandle h) const { return _pipelines.alive(h); }

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
    if (_pipes.cacheActive()) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s, 启动时 %zu B",
                      _pipes.cacheFromDisk() ? "loaded" : "new",
                      _pipes.cacheBytes());
        caps.details.push_back({"pipeline cache", buf});
    } else {
        caps.details.push_back({"pipeline cache", "disabled"});
    }
    return caps;
}

} // namespace rhi
