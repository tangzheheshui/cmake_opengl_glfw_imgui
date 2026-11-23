//
//  VkCommandList.h
//  RHI Vulkan 后端 - 命令录制实现（直译器）
//
#ifndef VkCommandList_h
#define VkCommandList_h

#include "../IRHICommandList.h"
#include "VkCommon.h"

namespace rhi {

class VulkanDevice;

class VkCommandList final : public IRHICommandList {
public:
    explicit VkCommandList(VulkanDevice* device);

    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;

    void bindPipeline(PipelineHandle pipeline) override;
    void setUniform(const char* name, const UniformValue& value) override;
    void setDrawUniforms(UniformBlockHandle block, const void* data, uint32_t size) override;
    void bindVertexBuffer(BufferHandle buffer, uint32_t binding = 0) override;
    void bindIndexBuffer(BufferHandle buffer) override;
    void bindTexture(TextureHandle texture, uint32_t slot = 0) override;
    void setViewport(float x, float y, float w, float h) override;

    void draw(uint32_t vertexCount, uint32_t firstVertex = 0) override;
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0) override;

    void pushDebugLabel(const char* label) override;   // M3: no-op（需 debug 扩展）
    void popDebugLabel() override;
    uint64_t nativeCommandBuffer() const override;

private:
    void applyPushConstants();

    VulkanDevice* _device = nullptr;

    // pipeline layout 由 VulkanDevice 持有（所有管线共用，固定 push 布局），
    // 通过 _device->pipelineLayout() 访问

    PipelineHandle _curPipeline;
    BufferHandle   _curVertexBuffer;
    BufferHandle   _curIndexBuffer;
    bool           _inRenderPass = false;

    UniformBlockHandle _curUniformBlock;        // 当前绘制使用的 uniform block（默认为 device 默认块）

    float _pushData[32] = {};   // 128B push constant 填充区
    bool  _pushDirty    = false;
};

} // namespace rhi

#endif /* VkCommandList_h */
