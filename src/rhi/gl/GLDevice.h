//
//  GLDevice.h
//  RHI OpenGL 后端 - 设备实现
//
//  定位：把 Vulkan 语义的显式调用"翻译"回 GL 状态机。
//  GLSL 编译由 rhi/gl/GLProgram 承载（L0 内聚，不再依赖 render 层）。
//
#ifndef GLDevice_h
#define GLDevice_h

#include "../IRHIDevice.h"
#include "../detail/DeletionQueue.h"
#include "GLProgram.h"
#include <memory>
#include <string>
#include <vector>

#include <glad/glad.h>

namespace rhi {

class GLCommandList;

// 后端内部资源表
struct GLBuffer {
    GLuint   id = 0;
    uint32_t usage = 0;
    uint32_t size = 0;
};

struct GLTexture {
    GLuint id = 0;
    bool   cubemap = false;   // 立方体贴图（绑定时用 GL_TEXTURE_CUBE_MAP 目标）
    // B4：readback 需要的尺寸/格式（回读按创建格式选取 glGetTexImage 参数）
    uint32_t width  = 0;
    uint32_t height = 0;
    Format   format = Format::RGBA8_UNorm;
    // B2：显式关联的独立采样器（attachSampler 设置；默认 invalid 时
    // bindTexture 保持既有行为——只依赖纹理对象上烘焙的 glTexParameteri 参数）
    SamplerHandle sampler;
};

// B4：离屏渲染目标（FBO + 颜色纹理 + 深度 RBO）。
// 颜色纹理复用 GLTexture（createTexture），采样/回读通用。
struct GLRenderTarget {
    GLuint         fbo = 0;
    TextureHandle  colorTex;
    GLuint         depthRbo = 0;
    uint32_t       width  = 0;
    uint32_t       height = 0;
    bool           alive  = true;
};

// B2：独立采样器对象（glGenSamplers + glSamplerParameter*）。
// 与纹理对象解耦：同一采样器可复用到多个纹理单元；绑定单元后覆盖该单元
// 所绑纹理的烘焙采样参数（GL 采样器对象语义）。
struct GLSampler {
    GLuint      id = 0;
    SamplerDesc desc;   // 留档（调试/对比 VK 端 sampler 一致性）
};

struct GLPipeline {
    std::unique_ptr<GLProgram> program;   // L0：RHI 自有 GLSL 编译器（原 render/shader.h）
    GraphicsPipelineDesc desc;
};

class GLDevice final : public IRHIDevice {
public:
    // C1a：宿主已创建 GL 上下文并 current；RHI 不创建窗口/上下文。
    // desc.window.swapBuffers 由宿主提供（present 用）。
    explicit GLDevice(const DeviceDesc& desc);
    ~GLDevice() override;

    Backend    backend() const override { return Backend::OpenGL; }
    DeviceInfo info() const override;

    // B2：结构化设备能力（glGetIntegerv/glGetString 惰性查询，启动时不打印；
    // 与 VK 端 VkCaps::toCapabilities 同风格的中文 details 表）
    DeviceCapabilities capabilities() const override;

    BufferHandle   createBuffer(const BufferDesc& desc) override;
    UniformBlockHandle createUniformBlock(uint32_t size) override;
    TextureHandle  createTexture(const TextureDesc& desc) override;
    PipelineHandle createPipeline(const GraphicsPipelineDesc& desc) override;
    void updateBuffer(BufferHandle buffer, const void* data,
                      uint32_t size, uint32_t offset = 0) override;

    // v2 对象级销毁（B0）：槽位保留为失效，句柄 id 不复用
    void destroyBuffer(BufferHandle) override;
    void destroyTexture(TextureHandle) override;
    void destroyPipeline(PipelineHandle) override;

    // ---- v2 独立采样器（B2；接口在 IRHIDevice，此前 GL 返回空实现）----
    SamplerHandle createSampler(const SamplerDesc& desc) override;
    void destroySampler(SamplerHandle h) override;
    // 把显式创建的独立采样器关联到纹理（B2：GL 后端内部扩展，不在冻结契约内；
    // 关联后 GLCommandList::bindTexture 对该纹理优先 glBindSampler，
    // 未关联的纹理保持既有行为——只用纹理对象上烘焙的 glTexParameteri）。
    // sampler invalid = 解除关联。
    void attachSampler(TextureHandle texture, SamplerHandle sampler);

    // v2 离屏渲染目标 + 同步回读（B4）
    RenderTargetHandle createRenderTarget(const RenderTargetDesc& desc) override;
    void destroyRenderTarget(RenderTargetHandle) override;
    TextureHandle renderTargetColorTexture(RenderTargetHandle) const override;
    bool readbackTexture(TextureHandle, void* outData, size_t outSize,
                         const ReadbackDesc& = {}) override;

    bool beginFrame() override;   // V3-0：帧号推进 + 延迟销毁队列 flush
    void endFrame()   override {}
    void waitIdle()   override;

    // ---- C1a：尺寸推送 + 呈现（RHI 不回查窗口系统）----
    void onResize(int width, int height) override;
    FramebufferSize framebufferSize() const override { return _size; }
    void present() override;   // 调 desc.window.swapBuffers（宿主回调）

    IRHICommandList* commandList() override;

    uint64_t nativeHandle(RhiHandle handle) const override;

    // 内部访问（GLCommandList 使用）
    const GLBuffer&   buffer(BufferHandle h) const;
    const GLTexture&  texture(TextureHandle h) const;   // 注意：GL 的 buffer/texture 是独立 id 空间，不可混用
    const GLSampler&  sampler(SamplerHandle h) const;
    const GLPipeline& pipeline(PipelineHandle h) const;
    const GLRenderTarget& renderTarget(RenderTargetHandle h) const;
    // B2：设备上是否存在显式创建的独立采样器。false 时 bindTexture 完全
    // 不触碰 GL_SAMPLER_BINDING 状态，保证遗留路径行为零变化。
    bool hasSamplers() const { return !_samplers.empty(); }

    // C1a：构造期有效性（上下文缺失/非法 desc 时 false，工厂据此报错）
    bool initialized() const { return _initialized; }

    // V3-0：销毁走延迟队列（帧在飞的资源不得立即删）。供析构兜底 flushAll。
    detail::DeletionQueue& deletionQueue() { return _delQueue; }

private:
    // C1a：RHI 不持有窗口——尺寸与交换回调来自 DeviceDesc（宿主职责）
    std::string _shaderRootPath;
    FramebufferSize _size;
    NativeWindow    _window;      // 宿主交换回调（present 用；非 GLFW 类型）
    bool _initialized = false;
    // V3-0：GL 无 fence，帧号由 beginFrame 推进（GL 路径 RenderSystem 每帧调用；
    // 遗留叠加路径的多次调用幂等无副作用）
    uint64_t _frameCount = 0;
    detail::DeletionQueue _delQueue;

    // 句柄 id = 数组下标 + 1（0 保留为 invalid）
    std::vector<GLBuffer>   _buffers;
    std::vector<GLTexture>  _textures;
    std::vector<GLSampler>  _samplers;   // B2 独立采样器
    std::vector<GLPipeline> _pipelines;
    std::vector<GLRenderTarget> _renderTargets;   // B4

    std::unique_ptr<GLCommandList> _commandList;
};

} // namespace rhi

#endif /* GLDevice_h */
