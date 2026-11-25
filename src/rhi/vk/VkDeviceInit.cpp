//
//  VkDeviceInit.cpp —— 实例/设备/surface/初始化
//  （从 VkDevice.cpp 按职责拆出，逻辑不变）
//
#include "VkDevice.h"
#include "VkDeviceInternal.h"
#include "../FormatInfo.h"
#include "VkCommandList.h"
#include "log/LogMacros.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rhi {

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
    // V3-13：句柄表槽位复用自检（分配→释放→再分配应复用同槽、旧句柄失效）
    if (!_buffers.debugSelfCheck()) {
        ENGINE_LOG_ERROR("[VK] HandleTable reuse self-check FAILED");
    }

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
    _commandList = std::make_unique<VkCommandList>(this, true);
    if (!_commandList->initCommandBuffers(_frames.slotCount())) {
        ENGINE_LOG_ERROR("[VK] primary command list init failed");
    }
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
    if (_mlWorker.joinable()) _mlWorker.join();
    if (_mlBuf) {
        vkDestroyBuffer(_device, _mlBuf, nullptr);
        _memAlloc.free(_mlMem);
        _mlBuf = VK_NULL_HANDLE;
    }
    _extraLists.clear();       // 附加列表释放自己的命令缓冲
    _commandList.reset();
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
                           findSupportedDepthFormat(_physicalDevice), _desc.msaaSamples,
                           &_memAlloc, nameFn)) {
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
    if (!_frames.init(_device, VkFrames::kDefaultSlots, nameFn)) {
        ENGINE_LOG_ERROR("[VK] frame sync objects init failed");
    }
}

void VulkanDevice::createDescriptorObjects() {
    const uint32_t minAlign = static_cast<uint32_t>(
        std::max<VkDeviceSize>(256, _caps.props.limits.minUniformBufferOffsetAlignment));
    VkDescriptors::NameFn nameFn = [this](VkObjectType type, uint64_t handle, const char* name) {
        setObjName(type, handle, name);
    };
    // 采样槽数：set0 + N 个纹理槽必须 <= maxBoundDescriptorSets（规范最低保证 4，
    // 桌面卡通常 32，Intel/MoltenVK 可能 8）。按上限裁，不够时只减少槽位，不崩。
    const uint32_t boundSets = _caps.props.limits.maxBoundDescriptorSets;
    uint32_t texSlots = (boundSets > 1) ? (boundSets - 1) : 1;
    if (texSlots > kMaxTexSlots) texSlots = kMaxTexSlots;
    ENGINE_LOG_INFO("[VK] texture slots = %u (maxBoundDescriptorSets=%u)",
                    texSlots, boundSets);
    if (!_descs.init(_device, minAlign, &_memAlloc, texSlots, nameFn)) {
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



} // namespace rhi
