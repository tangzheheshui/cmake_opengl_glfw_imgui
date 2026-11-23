//
//  IRHICommandList.h
//  RHI 抽象层 - 命令录制接口
//
//  使用模式（每帧）：
//    auto* cmd = device->commandList();
//    cmd->beginRenderPass(passDesc);          // 对应一个帧级 Pass（阴影/主渲染/拾取）
//      cmd->bindPipeline(pipe);               // 对应 Pass 内的绘制策略
//      cmd->setUniform("uMVP", value);        // bind 之后、draw 之前，任意多次
//      cmd->bindVertexBuffer(vb);
//      cmd->bindIndexBuffer(ib);
//      cmd->drawIndexed(indexCount);
//    cmd->endRenderPass();
//
#ifndef IRHICommandList_h
#define IRHICommandList_h

#include "RhiTypes.h"

namespace rhi {

class IRHICommandList {
public:
    virtual ~IRHICommandList() = default;

    // ---- 渲染 Pass ----
    virtual void beginRenderPass(const RenderPassDesc& desc) = 0;
    virtual void endRenderPass() = 0;

    // ---- 管线与资源绑定 ----
    virtual void bindPipeline(PipelineHandle pipeline) = 0;

    // 按 GLSL uniform 名字更新数据块。
    // GL 后端：draw 时翻译为 glUniform*；
    // Vulkan 后端：攒入当前 draw 的 push constant / dynamic UBO。
    virtual void setUniform(const char* name, const UniformValue& value) = 0;

    virtual void bindVertexBuffer(BufferHandle buffer, uint32_t binding = 0) = 0;
    virtual void bindIndexBuffer(BufferHandle buffer) = 0;

    // ---- 绘制级 uniform 数据块 ----
    // 将一次绘制所需的 uniform 数据（灯光/材质/相机等）一次性提交。
    // GL 后端：引擎按约定字段名拆解为 glUniform*；
    // Vulkan 后端：整块写入共享 UBO（固定布局，见 docs/RHI_DESIGN.md 的 FrameData）。
    // 两后端的着色器各自声明匹配的数据视图。
    virtual void setDrawUniforms(UniformBlockHandle block, const void* data, uint32_t size) = 0;

    // ---- 纹理绑定 ----
    // 绑定纹理到采样槽位（描述符槽 / GL 纹理单元）。
    // GL 后端：绑到 GL_TEXTURE0+slot；采样器 uniform 由业务侧 setInt 关联。
    // Vulkan 后端：写入 combined image sampler 描述符集的对应 binding。
    virtual void bindTexture(TextureHandle texture, uint32_t slot = 0) = 0;

    virtual void setViewport(float x, float y, float w, float h) = 0;

    // ---- 绘制 ----
    virtual void draw(uint32_t vertexCount, uint32_t firstVertex = 0) = 0;
    virtual void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0) = 0;

    // ---- 调试 ----
    // 插入调试标记（GL: glPushDebugGroup；Vulkan: vkCmdDebugMarkerBegin）
    // RenderDoc / Xcode GPU captures 中可见，排障时用。
    virtual void pushDebugLabel(const char* label) = 0;

    // 原生命令缓冲（第三方库如 ImGui Vulkan backend 在当前帧内渲染用）
    virtual uint64_t nativeCommandBuffer() const { return 0; }
    virtual void popDebugLabel() = 0;
};

} // namespace rhi

#endif /* IRHICommandList_h */
