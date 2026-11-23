//
//  VkCommon.h
//  RHI Vulkan 后端 - 内部公共定义（仅 VulkanDevice/VkCommandList/factory 使用）
//
#ifndef VkCommon_h
#define VkCommon_h

#include <vulkan/vulkan.h>
#include "log/LogMacros.h"
#include <cstdlib>

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

// 后端内部 Buffer 资源（M3：HOST_VISIBLE + 持久映射，适合小数据量）
struct VkBufferRes {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void*          mapped = nullptr;
    uint32_t       size   = 0;
};

// 后端内部纹理资源（M4：DEVICE_LOCAL + staging 一次性上传，无 mipmap）
struct VkTextureRes {
    VkImage        image   = VK_NULL_HANDLE;
    VkDeviceMemory memory  = VK_NULL_HANDLE;
    VkImageView    view    = VK_NULL_HANDLE;
    VkSampler      sampler = VK_NULL_HANDLE;
    uint32_t       width   = 0;
    uint32_t       height  = 0;
};

// 绘制级 uniform 块（M4.2）：独立 UBO + 独立描述符集（避免共享 UBO 逐帧覆盖）
struct VkUniformBlockRes {
    VkBuffer       buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void*          mapped = nullptr;
    VkDescriptorSet set   = VK_NULL_HANDLE;
    uint32_t       size   = 0;
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
};

// push constant 布局常量（与 res/shaders_vk/*.vert|.frag 中的 PC 块一致）
constexpr uint32_t kPushMvpOffset  = 0;    // mat4
constexpr uint32_t kPushColorOffset = 64;  // vec4
constexpr uint32_t kPushTotalSize   = 128;   // M4.3: 天空盒需双 mat4（uViewMat@0 + uProjMat@64）
// 绘制级 uniform UBO 大小（engine::DrawUniforms = 160B，对齐预留）
constexpr uint32_t kDrawUniformsSize = 256;

} // namespace rhi

#endif /* VkCommon_h */
