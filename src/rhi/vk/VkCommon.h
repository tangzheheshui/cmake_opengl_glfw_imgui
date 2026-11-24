//
//  VkCommon.h
//  RHI Vulkan 后端 - 内部公共定义（仅 VulkanDevice/VkCommandList/factory 使用）
//
#ifndef VkCommon_h
#define VkCommon_h

#include <vulkan/vulkan.h>
// windows.h（经 vulkan_win32.h 引入）里 wingdi.h 有打印机 API `DeviceCapabilities`，
// 且 #define DeviceCapabilities DeviceCapabilitiesW，会顶掉 RhiTypes.h 的同名结构体。
// 这里 #undef 掉，让后续 IRHIDevice.h/VkDevice.h 里的 DeviceCapabilities 指向我们的类型。
#undef DeviceCapabilities
#include "../RhiTypes.h"
#include "log/LogMacros.h"
#include "VkMemoryAllocator.h"
#include <cstdlib>
#include <vector>

namespace rhi {

// Vulkan 调用检查：失败打印并中止（M3 阶段严格模式，便于尽早暴露问题）
#define VK_CHECK(call)                                                        \
    do {                                                                      \
        VkResult _r = (call);                                                 \
        if (_r != VK_SUCCESS) {                                               \
            ENGINE_LOG_ERROR("[VK] %s failed (%d) at %s:%d",                  \
                             #call, (int)_r, __FILE__, __LINE__);             \
            std::abort();                                                     \
        }                                                                     \
    } while (0)

// V3-0：资源创建路径的失败语义——打印 + return {}（invalid 句柄），不中止进程。
// 仅限返回句柄类型的 create* 函数内使用；帧路径/设备初始化仍用 VK_CHECK（abort）。
#define VK_CHECK_RET(call)                                                    \
    do {                                                                      \
        const VkResult _r_ = (call);                                          \
        if (_r_ != VK_SUCCESS) {                                              \
            ENGINE_LOG_ERROR("[VK] %s failed (%d) at %s:%d",                  \
                             #call, (int)_r_, __FILE__, __LINE__);            \
            return {};                                                        \
        }                                                                     \
    } while (0)

// 后端内部 Buffer 资源（M3：HOST_VISIBLE + 持久映射，适合小数据量）
struct VkBufferRes {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void*          mapped = nullptr;
    uint32_t       size   = 0;
    VkMemAlloc     memAlloc;   // E1：子分配上下文（free 用；专用/页内皆可）
};

// 后端内部纹理资源（M4：DEVICE_LOCAL + staging 一次性上传，无 mipmap）
struct VkTextureRes {
    VkImage        image   = VK_NULL_HANDLE;
    VkDeviceMemory memory  = VK_NULL_HANDLE;
    VkImageView    view    = VK_NULL_HANDLE;
    VkSampler      sampler = VK_NULL_HANDLE;
    uint32_t       width   = 0;
    uint32_t       height  = 0;
    // B4：字节/像素（readback 计算区域字节数用）
    uint32_t       bpp     = 0;
    // B4：布局跟踪（dynamic rendering 无自动布局转换；上传后 = SHADER_READ_ONLY，
    // 作 RT 颜色附件时 COLOR_ATTACHMENT ↔ SHADER_READ_ONLY 往返，回读时 TRANSFER_SRC）
    VkImageLayout  layout  = VK_IMAGE_LAYOUT_UNDEFINED;
    VkMemAlloc     memAlloc;   // E1
};

// B4：离屏渲染目标。颜色附件复用纹理机制（SAMPLED|COLOR_ATTACHMENT，
// 采样/描述符/bindTexture 全部通用）；深度附件独立 image/view
// （storeOp=DONT_CARE + 每次 begin CLEAR，跨 Pass 不保内容）。
struct VkRenderTargetRes {
    TextureHandle  colorTex;                            // 颜色附件（VkTextureRes）
    VkImage        depthImage  = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkMemAlloc     depthMemAlloc;                       // E1
    VkImageView    depthView   = VK_NULL_HANDLE;
    uint32_t       width  = 0;
    uint32_t       height = 0;
    VkImageLayout  depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    bool           alive  = true;   // 对象级销毁：句柄 id 不复用，dead 槽位保留
};

// 绘制级 uniform 块（M4.2）：独立 UBO + 独立描述符集（避免共享 UBO 逐帧覆盖）
struct VkUniformBlockRes {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void*          mapped = nullptr;
    VkDescriptorSet set   = VK_NULL_HANDLE;
    uint32_t       size   = 0;
    VkMemAlloc     memAlloc;   // E1
};

// B6：反射出的 push constant 成员（管线创建期从 SPIR-V 生成，
// setUniform 按此表按名定位偏移——替换字符串→偏移硬编码约定）
struct VkPipelineUniformInfo {
    char     name[32];
    uint32_t offset = 0;
    uint32_t size   = 0;
    bool     isMat4 = false;   // mat4 成员 setUniform 时做 Reversed-Z 修正
};

struct VkPipelineRes {
    VkPipeline       pipeline = VK_NULL_HANDLE;
    // v2 对象级销毁：destroyPipeline 后槽位保留（句柄 id 不复用），
    // swapchain 重建时跳过 dead 槽位，不"复活"已销毁的管线。
    bool             alive    = true;
    // M3 固定 push constant 布局的填充缓冲（见 VkCommandList）
    //  [0,64)  mat4 uMatrixMVP（顶点）
    //  [64,80) vec4 uColor   （片元，Vec3 写入时 alpha 填 1）
    float pushConstants[32] = {};
    // B6：反射 uniform 表（名字命中即按 offset 写 push；空表回退旧映射）
    std::vector<VkPipelineUniformInfo> uniformTable;
};

// push constant 布局常量（与 res/shaders_vk/*.vert|.frag 中的 PC 块一致）
constexpr uint32_t kPushMvpOffset  = 0;    // mat4
constexpr uint32_t kPushColorOffset = 64;  // vec4
constexpr uint32_t kPushTotalSize   = 128;   // M4.3: 天空盒需双 mat4（uViewMat@0 + uProjMat@64）
// 绘制级 uniform UBO 大小（engine::DrawUniforms = 160B，对齐预留）
constexpr uint32_t kDrawUniformsSize = 256;

} // namespace rhi

#endif /* VkCommon_h */
