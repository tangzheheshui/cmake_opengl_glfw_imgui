//
//  VulkanDevice.h
//  RHI Vulkan 后端 - 设备实现
//
//  M3 范围：
//  - instance（含 macOS portability 枚举）+ GLFW surface
//  - 物理设备/队列族选择 + logical device
//  - swapchain（FIFO/vsync）+ image views + framebuffers + render pass（无深度附件）
//  - 单命令缓冲逐帧录制，fence/semaphore 同步
//  - Buffer：HOST_VISIBLE 持久映射（小数据量够用，大缓冲 M4 换 staging 上传）
//  - Pipeline：SPIR-V（构建期 glslangValidator 编译 res/shaders_vk/）
//
#ifndef VkDevice_h
#define VkDevice_h

#include "../IRHIDevice.h"
#include "VkCommon.h"
#include "VkCaps.h"
#include <vector>
#include <string>
#include <functional>

struct GLFWwindow;

namespace rhi {

class VkCommandList;

class VulkanDevice final : public IRHIDevice {
public:
    VulkanDevice(GLFWwindow* window, const std::string& shaderRootPath);
    ~VulkanDevice() override;

    Backend    backend() const override { return Backend::Vulkan; }
    DeviceInfo info() const override;
    DeviceCapabilities capabilities() const override;   // A3/B0: 结构化能力 + 运行时状态表

    BufferHandle   createBuffer(const BufferDesc& desc) override;
    TextureHandle  createTexture(const TextureDesc& desc) override;
    PipelineHandle createPipeline(const GraphicsPipelineDesc& desc) override;
    UniformBlockHandle createUniformBlock(uint32_t size) override;
    void updateBuffer(BufferHandle buffer, const void* data,
                      uint32_t size, uint32_t offset = 0) override;

    // v2 对象级销毁（B0）：槽位保留为失效，句柄 id 不复用
    void destroyBuffer(BufferHandle) override;
    void destroyTexture(TextureHandle) override;
    void destroyPipeline(PipelineHandle) override;
    void destroyUniformBlock(UniformBlockHandle) override;

    void beginFrame() override;
    void endFrame()   override;
    void waitIdle()   override;
    void onResize(int width, int height) override;

    IRHICommandList* commandList() override;
    uint64_t nativeHandle(RhiHandle handle) const override;
    bool nativeHandles(NativeHandles& out) const override;

    // 内部访问（VkCommandList 使用）
    GLFWwindow* window() const { return _window; }
    const std::string& shaderRootPath() const { return _shaderRootPath; }
    ::VkDevice           vkDeviceHandle() const { return _device; }
    VkExtent2D       swapchainExtent() const { return _swapchainExtent; }
    VkFormat         swapchainFormat() const { return _swapchainFormat; }
    VkFormat         depthFormat() const { return _depthFormat; }
    bool             dynamicRendering() const { return _dynamicRendering; }
    PFN_vkCmdBeginRenderingKHR beginRenderingFn() const { return _vkBeginRendering; }
    PFN_vkCmdEndRenderingKHR   endRenderingFn() const { return _vkEndRendering; }

    // 后端内部资源访问（句柄 id = 下标+1，同 GL 后端约定）
    const VkBufferRes&   bufferRes(BufferHandle h) const { return _buffers[h.id - 1]; }
    const VkPipelineRes& pipelineRes(PipelineHandle h) const { return _pipelines[h.id - 1]; }
    const VkTextureRes&  textureRes(TextureHandle h) const { return _textures[h.id - 1]; }
    const VkUniformBlockRes& uniformBlockRes(UniformBlockHandle h) const { return _uniformBlocks[h.id - 1]; }
    UniformBlockHandle defaultUniformBlock() const { return _defaultUniformBlock; }
    VkDescriptorSetLayout texSetLayout() const { return _texSetLayout; }
    VkDescriptorPool      texDescriptorPool() const { return _texDescriptorPool; }
    VkPipelineLayout      pipelineLayout() const { return _pipelineLayout; }

    // B1：dynamic rendering 下由 command list 调用的布局屏障与附件视图
    VkImageView currentColorView() const;          // 当前 swapchain image view
    VkImageView depthImageView() const { return _depthView; }
    // beginRenderPass 前的图像布局屏障（color → COLOR_ATTACHMENT / depth 首帧转换）
    void recordBeginPassBarriers(VkCommandBuffer cb, bool clearColor, bool clearDepth);
    // endFrame 提交前的 color → PRESENT_SRC 转换（dynamic rendering 无自动 finalLayout）
    void recordPresentBarrier(VkCommandBuffer cb);
    VkCommandBuffer commandBuffer() const { return _commandBuffer; }
    uint32_t currentImageIndex() const { return _imageIndex; }
    void bindInstanceVertexBuffer(VkCommandBuffer cb, BufferHandle h) const;

private:
    friend class VkCommandList;
    void createInstance();
    void createSurface();
    void pickPhysicalDevice();
    void createLogicalDevice();
    void createSwapchainObjects();   // swapchain + views + depth（B1：无 renderpass/framebuffer）
    void destroySwapchainObjects();
    void createCommandObjects();
    void createSyncObjects();
    // A3: Pipeline Cache 持久化（创建/加载/退出时保存；为优化项，失败不致命）
    void createPipelineCache();
    void savePipelineCache();
    void createDescriptorObjects();  // 纹理描述符池 + set layout（M4）
    // A1: Validation Layer + Debug Utils
    void setupDebugValidation(std::vector<const char*>& exts, std::vector<const char*>& layers);
    void createDebugMessenger();
    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData);
    void setObjName(VkObjectType type, uint64_t handle, const char* name);   // 对象命名（RenderDoc/capture 可读）
    void deferGpuWork(std::function<void(VkCommandBuffer)> fn, std::function<void()> onComplete = nullptr);
    void drainDeferredGpuWork();
    void createWhiteFallbackTexture();
    VkFormat findDepthFormat() const;
    // 一次性提交：临时命令缓冲上执行录制回调并等待完成（纹理上传等初始化用）
    void submitOneTime(std::function<void(VkCommandBuffer)> record, std::function<void()> onComplete = nullptr);

    bool beginFrameInternal();       // 失败（swapchain 过期）返回 false，本帧跳过

    GLFWwindow*  _window = nullptr;
    std::string  _shaderRootPath;

    VkInstance       _instance       = VK_NULL_HANDLE;
    VkSurfaceKHR     _surface        = VK_NULL_HANDLE;
    VkPhysicalDevice _physicalDevice = VK_NULL_HANDLE;
    ::VkDevice           _device         = VK_NULL_HANDLE;
    uint32_t         _queueFamily    = 0;
    VkQueue          _queue          = VK_NULL_HANDLE;

    VkSwapchainKHR             _swapchain      = VK_NULL_HANDLE;
    VkFormat                   _swapchainFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR            _swapchainColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkExtent2D                 _swapchainExtent = {0, 0};
    std::vector<VkImage>       _swapchainImages;
    std::vector<VkImageView>   _swapchainViews;
    // B1：dynamic rendering 取代 renderpass/framebuffer。
    // 布局跟踪：dynamic rendering 不做附件布局自动转换，由后端逐帧记录屏障；
    // color 每 image 一份（present 后 = PRESENT_SRC），depth 全局一份。
    std::vector<VkImageLayout> _swapchainLayouts;   // 与 _swapchainImages 对齐
    VkImageLayout              _depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat                   _depthFormat = VK_FORMAT_UNDEFINED;
    VkImage                    _depthImage  = VK_NULL_HANDLE;
    VkDeviceMemory             _depthMemory = VK_NULL_HANDLE;
    VkImageView                _depthView   = VK_NULL_HANDLE;

    VkCommandPool   _commandPool   = VK_NULL_HANDLE;
    VkCommandBuffer _commandBuffer = VK_NULL_HANDLE;

    // A3: 全部管线共用的 pipeline layout（固定 push constant 布局，创建一次）
    VkPipelineLayout _pipelineLayout = VK_NULL_HANDLE;

    // A3: 物理设备能力（初始化时探测一次）+ Pipeline Cache 持久化状态
    VkCaps         _caps;
    VkPipelineCache _pipelineCache = VK_NULL_HANDLE;
    std::string    _pipelineCachePath;
    bool           _pipelineCacheFromDisk = false;   // 本次启动是否成功加载了旧缓存
    size_t         _pipelineCacheLoadedBytes = 0;

    // 纹理描述符（M4）+ 绘制级 uniform UBO（M4.2）
    VkDescriptorSetLayout _texSetLayout      = VK_NULL_HANDLE;
    VkDescriptorPool      _texDescriptorPool = VK_NULL_HANDLE;
    // A1: 验证层与调试工具
    bool                  _validationEnabled = false;
    bool                  _descriptorIndexing = false;
    bool                  _debugUtilsEnabled = false;
    VkDebugUtilsMessengerEXT _debugMessenger = VK_NULL_HANDLE;
    PFN_vkSetDebugUtilsObjectNameEXT _setNameFn = nullptr;
    PFN_vkCreateDebugUtilsMessengerEXT _createDebugMessengerFn = nullptr;
    PFN_vkDestroyDebugUtilsMessengerEXT _destroyDebugMessengerFn = nullptr;
    std::vector<VkTextureRes> _textures;
    TextureHandle _whiteTex;
    TextureHandle _whiteCubeTex;
    std::vector<VkUniformBlockRes> _uniformBlocks;
    UniformBlockHandle _defaultUniformBlock;   // 未显式持有 block 的绘制使用

    VkSemaphore _semImageAvailable = VK_NULL_HANDLE;
    // 每个 swapchain image 一个 render-done semaphore：
    // 单个 semaphore 在不同 image 间复用会触发 VUID-vkQueueSubmit-pSignalSemaphores-00067
    std::vector<VkSemaphore> _semRenderDone;
    VkFence     _fenceInFlight     = VK_NULL_HANDLE;

    uint32_t _imageIndex     = 0;
    bool     _frameActive    = false;
    bool     _frameSkipped   = false;
    bool     _needsRecreate  = false;

    std::vector<VkBufferRes>   _buffers;
    std::vector<VkPipelineRes> _pipelines;
    // 帧内请求的一次性 GPU 工作（纹理上传等）：render pass 活跃时提交会触发
    // DEVICE_LOST（MoltenVK），统一延迟到下一帧 beginFrame 执行
    struct DeferredGpuWork {
        std::function<void(VkCommandBuffer)> record;
        std::function<void()> onComplete;   // 提交并等待完成后执行（如 staging 销毁）
    };
    std::vector<DeferredGpuWork> _deferredGpu;

    // B1：Dynamic Rendering（VK_KHR_dynamic_rendering；Vulkan 1.3 core）。
    // 管线不再绑定 renderPass → resize 重建 swapchain 无需重建 PSO。
    // 1.2 设备走扩展 + KHR 函数指针；1.3 设备走 core 特性（函数仍用 KHR 别名，
    // 已提升扩展在驱动中保留 KHR 入口点）。
    bool _dynamicRendering = false;
    uint32_t _deviceApiVersion = 0;
    PFN_vkCmdBeginRenderingKHR _vkBeginRendering = nullptr;
    PFN_vkCmdEndRenderingKHR   _vkEndRendering = nullptr;

    std::unique_ptr<VkCommandList> _commandList;
    std::string _adapterName;
};

} // namespace rhi

#endif /* VkDevice_h */
