//
//  GLCommandList.h
//  RHI OpenGL 后端 - 命令录制实现
//
//  实现策略（M2）：
//  - VAO 按 (pipeline, vertexBuffer) 组合缓存，bind 后每次 draw 显式重绑
//    ELEMENT_ARRAY_BUFFER（它是 VAO 状态，无法跨 VAO 复用判断）
//  - setUniform 先攒队列，draw 时统一翻译为 glUniform*
//
#ifndef GLCommandList_h
#define GLCommandList_h

#include "../IRHICommandList.h"
#include <map>
#include <string>
#include <utility>
#include <vector>
#include <glad/glad.h>

namespace rhi {

class GLDevice;

class GLCommandList final : public IRHICommandList {
public:
    explicit GLCommandList(GLDevice* device);
    ~GLCommandList() override;

    void beginRenderPass(const RenderPassDesc& desc) override;
    void endRenderPass() override;

    void bindPipeline(PipelineHandle pipeline) override;
    void setUniform(const char* name, const UniformValue& value) override;
    void setDrawUniforms(UniformBlockHandle block, const void* data, uint32_t size) override;
    void bindVertexBuffer(BufferHandle buffer, uint32_t binding = 0) override;
    void bindIndexBuffer(BufferHandle buffer) override;
    void bindTexture(TextureHandle texture, uint32_t slot = 0) override;
    void setViewport(float x, float y, float w, float h) override;
    void setScissor(int x, int y, int w, int h) override;   // C2：坐标 = 目标左上原点

    void draw(uint32_t vertexCount, uint32_t firstVertex = 0) override;
    void drawIndexed(uint32_t indexCount, uint32_t instanceCount = 1, uint32_t firstIndex = 0) override;

    void pushDebugLabel(const char* label) override;
    void popDebugLabel() override;

    // V3-0：buffer/pipeline 销毁后清理引用它的 VAO 缓存条目。
    // VAO 的属性指针/绑定在创建期固化为 GL 对象名，源资源删除后复用该 VAO
    // 会画出垃圾（悬空引用）。
    void invalidateVAOForBuffer(uint32_t bufferId);
    void invalidateVAOForPipeline(uint32_t pipelineId);

private:
    // 按 (pipeline, vertexBuffer) 组合缓存 VAO（M2：替换 v1 的单 VAO 重配置）
    // key: <pipelineId, vertexBufferId>，value: 配置好的 VAO
    GLuint obtainVAO();

    void flushUniforms();           // 攒的 uniform 翻译成 glUniform*

    // GL 是状态机，而 Vulkan 语义里 Pass 边界外的状态不该被 Pass 内的管线改变。
    // beginRenderPass 保存、endRenderPass 恢复这些状态，
    // 保证 RHI Pass 对周边遗留 GL 代码"状态中性"（否则 depthMask 等会泄漏）。
    struct GLStateSnapshot {
        GLboolean depthTest  = GL_FALSE;
        GLboolean depthMask  = GL_TRUE;
        GLboolean blend      = GL_FALSE;
        GLboolean cullFace   = GL_FALSE;
        GLint     cullMode   = GL_BACK;
        GLint     framebuffer = 0;   // D2：FBO 绑定纳入 Pass 边界（RT pass 后恢复，
                                     // 否则 ImGui 等宿主渲染会画进离屏 RT）
        GLint     viewport[4] = {0, 0, 0, 0};   // C2：视口纳入 Pass 边界
        GLboolean scissorTest = GL_FALSE;       // C2：裁剪测试纳入 Pass 边界
        GLint     scissorBox[4] = {0, 0, 0, 0};
    };
    void saveState(GLStateSnapshot& s);
    void restoreState(const GLStateSnapshot& s);

    GLDevice* _device = nullptr;

    PipelineHandle _curPipeline;
    BufferHandle   _curVertexBuffer;
    BufferHandle   _curInstanceBuffer;   // 实例率数据流（binding1）
    BufferHandle   _curIndexBuffer;

    // VAO 缓存：同一管线+同一顶点流+同一实例流复用同一 VAO
    std::map<std::tuple<uint32_t, uint32_t, uint32_t>, GLuint> _vaoCache;

    std::vector<std::pair<std::string, UniformValue>> _pendingUniforms;

    GLStateSnapshot _savedState;
    bool _stateSaved = false;
    int  _curTargetHeight = 0;   // C2：当前 pass 目标高度（scissor 坐标换算用）

    // B2：本次 Pass 内绑定过独立采样器的纹理单元（endRenderPass 统一解绑为 0，
    // 把"Pass 边界状态中性"约定扩展到 GL_SAMPLER_BINDING）
    bool _samplerSlotsTouched[32] = {};
};

} // namespace rhi

#endif /* GLCommandList_h */
