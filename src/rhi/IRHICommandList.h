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
    // Vulkan 后端：整块写入共享 UBO（固定布局，见 docs/4-RHI接口契约.md 的 FrameData）。
    // 两后端的着色器各自声明匹配的数据视图。
    virtual void setDrawUniforms(UniformBlockHandle block, const void* data, uint32_t size) = 0;

    // ---- 纹理绑定 ----
    // 绑定纹理到采样槽位（描述符槽 / GL 纹理单元）。
    // GL 后端：绑到 GL_TEXTURE0+slot；samplerName 非空时同步把该采样器 uniform
    //          指向 slot（GL 没有"描述符集"，采样器 unit 必须由 uniform 指定）。
    // Vulkan 后端：写入 combined image sampler 描述符集的对应 binding；
    //          samplerName 忽略（set 索引就是槽位）。
    virtual void bindTexture(TextureHandle texture, uint32_t slot = 0,
                             const char* samplerName = nullptr) = 0;

    virtual void setViewport(float x, float y, float w, float h) = 0;

    // 裁剪矩形（v2；C2 多视口用）。默认 = 整目标，后端在 C2 落地。
    virtual void setScissor(int x, int y, int w, int h) {
        (void)x; (void)y; (void)w; (void)h;
    }

    // 颜色空间写编码开关（GL：GL_FRAMEBUFFER_SRGB；VK 由附件格式驱动 → no-op）。
    // render 层不直接出现 gl*：场景 pass 默认编码（beginRenderPass 打开），
    // 宿主 UI（ImGui 保持 sRGB 字节外观）在 overlay 前关掉。
    virtual void setSrgbWrite(bool /*enabled*/) {}

    // 当前视口像素尺寸（最近一次 setViewport / beginRenderPass 的值）。
    // 屏幕空间展开类对象（宽线）据此换算像素→NDC，render 层不直接查 GL。
    virtual void viewportSize(uint32_t& w, uint32_t& h) const { w = 0; h = 0; }

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
