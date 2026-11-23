//
//  GLDevice.h
//  RHI OpenGL 后端 - 设备实现
//
//  定位：把 Vulkan 语义的显式调用"翻译"回 GL 状态机。
//  现有的 Shader 类（render/shader.h）直接复用，负责 GLSL 编译。
//
#ifndef GLDevice_h
#define GLDevice_h

#include "../IRHIDevice.h"
#include <memory>
#include <string>
#include <vector>

#include <glad/glad.h>

class Shader;   // render/shader.h，cpp 中包含

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
};

struct GLPipeline {
    std::unique_ptr<Shader> program;
    GraphicsPipelineDesc desc;
};

class GLDevice final : public IRHIDevice {
public:
    GLDevice(GLFWwindow* window, const std::string& shaderRootPath);
    ~GLDevice() override;

    Backend    backend() const override { return Backend::OpenGL; }
    DeviceInfo info() const override;

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

    void beginFrame() override {}
    void endFrame()   override {}
    void waitIdle()   override;

    IRHICommandList* commandList() override;

    uint64_t nativeHandle(RhiHandle handle) const override;

    // 内部访问（GLCommandList 使用）
    const GLBuffer&   buffer(BufferHandle h) const;
    const GLTexture&  texture(TextureHandle h) const;   // 注意：GL 的 buffer/texture 是独立 id 空间，不可混用
    const GLPipeline& pipeline(PipelineHandle h) const;
    GLFWwindow*       window() const { return _window; }

private:
    GLFWwindow* _window = nullptr;
    std::string _shaderRootPath;

    // 句柄 id = 数组下标 + 1（0 保留为 invalid）
    std::vector<GLBuffer>   _buffers;
    std::vector<GLTexture>  _textures;
    std::vector<GLPipeline> _pipelines;

    std::unique_ptr<GLCommandList> _commandList;
};

} // namespace rhi

#endif /* GLDevice_h */
