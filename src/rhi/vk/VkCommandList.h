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
    // primary=true 是设备的主列表（每帧最后提交）；false 是 createCommandList()
    // 拿到的附加列表（先于主列表提交）。
    explicit VkCommandList(VulkanDevice* device, bool primary = true);
    ~VkCommandList() override;

    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;

    void bindPipeline(PipelineHandle pipeline) override;
    void setUniform(const char* name, const UniformValue& value) override;
    void setDrawUniforms(UniformBlockHandle block, const void* data, uint32_t size) override;
    void bindVertexBuffer(BufferHandle buffer, uint32_t binding = 0) override;
    void bindIndexBuffer(BufferHandle buffer) override;
    void bindTexture(TextureHandle texture, uint32_t slot = 0) override;
    void setViewport(float x, float y, float w, float h) override;
    void setScissor(int x, int y, int w, int h) override;   // C2：VK 帧缓冲即左上原点，直接透传

    void draw(uint32_t vertexCount, uint32_t firstVertex = 0) override;
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0) override;

    void pushDebugLabel(const char* label) override;   // M3: no-op（需 debug 扩展）
    void popDebugLabel() override;
    uint64_t nativeCommandBuffer() const override;

    // V3-6：命令缓冲按帧槽位各一份（两帧在飞时不能共用）。
    // 设备在命令池就绪后调用一次；失败返回 false，该列表不可用。
    bool initCommandBuffers(uint32_t slots);
    // 每帧开头：重置并开始录制本槽位命令缓冲，同时把绑定缓存/uniform 偏移/
    // 采样槽按帧归零（新命令缓冲里旧绑定不再有效）。
    void beginFrameSlot(uint32_t slot);
    // 本帧是否录过东西（没录过的附加列表不提交）
    bool used() const { return _used; }
    void markUsed() { _used = true; }
    uint32_t slot() const { return _slot; }
    VkCommandBuffer commandBuffer() const {
        return _slot < _cmdBuffers.size() ? _cmdBuffers[_slot] : VK_NULL_HANDLE;
    }

private:
    void applyPushConstants();
    void bindDescriptorState();   // set0（动态偏移）+ set1..4（纹理槽），带去重

    VulkanDevice* _device = nullptr;

    // pipeline layout 由 VulkanDevice 持有（所有管线共用，固定 push 布局），
    // 通过 _device->pipelineLayout() 访问

    bool _primary = true;
    std::vector<VkCommandBuffer> _cmdBuffers;   // 下标 = 帧槽位
    uint32_t _slot = 0;
    bool     _used = false;

    PipelineHandle _curPipeline;
    BufferHandle   _curVertexBuffer;
    BufferHandle   _curIndexBuffer;
    bool           _inRenderPass = false;
    RenderTargetHandle _curRenderTarget;   // B4：当前离屏 RT（invalid = swapchain）

    // ---- V3-2 绑定状态 ----
    // 采样槽：bindTexture 只记录，draw 时解析成纹理各自的描述符集
    TextureHandle _texSlots[kMaxTexSlots];
    // 当前绘制数据块在帧内环里的偏移（setDrawUniforms 更新）
    uint32_t      _uniformOffset = 0;
    // 去重缓存：和上次绑定完全一致就跳过 vkCmdBindDescriptorSets
    VkDescriptorSet _lastSets[1 + kMaxTexSlots] = {};
    uint32_t        _lastOffset = UINT32_MAX;

    float _pushData[32] = {};   // 128B push constant 填充区
    bool  _pushDirty    = false;
};

} // namespace rhi

#endif /* VkCommandList_h */
