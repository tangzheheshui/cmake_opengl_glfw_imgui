//
//  VkCommandList.h
//  RHI Vulkan 后端 - 命令录制实现（直译器）
//
#ifndef VkCommandList_h
#define VkCommandList_h

#include "../IRHICommandList.h"
#include "../detail/FrameUniformRing.h"
#include "VkCommon.h"
#include <thread>

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
    // V3-12：记录本 Pass 实际附件格式（beginRenderPass 时算一次，bindPipeline 比对）
    void recordPassFormats(bool hasColor, Format colorFormat, bool hasDepth, Format depthFormat,
                           uint32_t samples);

    VulkanDevice* _device = nullptr;

    // pipeline layout 由 VulkanDevice 持有（所有管线共用，固定 push 布局），
    // 通过 _device->pipelineLayout() 访问

    bool _primary = true;
    // V3-14：每条列表自己的命令池。Vulkan 规定同一个命令池不能跨线程并发录制，
    // 多线程录制的前提就是"一线程一列表一命令池"。
    VkCommandPool _pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> _cmdBuffers;   // 下标 = 帧槽位
    // V3-14：每条列表自己按帧槽位持有一条 uniform 环 + 对应的 set0。
    // 这是多线程录制的前提——两条列表各写各的环，不抢游标。
    std::vector<detail::FrameUniformRing> _rings;
    std::vector<VkDescriptorSet> _frameSets;
    uint32_t _slot = 0;
    bool     _used = false;

    // 调试期线程归属检查：一条列表同时只能被一个线程录制
    std::thread::id _ownerThread;
    bool            _ownerSet = false;
    void claimThread();

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

    // V3-12：本 Pass 实际附件格式 + 与当前管线不匹配时跳过绘制
    bool   _passHasColor = false;
    bool   _passHasDepth = false;
    Format _passColorFormat = Format::BGRA8_SRGB;
    Format _passDepthFormat = Format::Depth32F;
    uint32_t _passSamples   = 1;
    bool   _formatMismatch  = false;

    float _pushData[32] = {};   // 128B push constant 填充区
    bool  _pushDirty    = false;
};

} // namespace rhi

#endif /* VkCommandList_h */
