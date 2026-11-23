//
//  GLDevice.cpp
//
#include "GLDevice.h"
#include "GLCommandList.h"
#include "shader.h"                 // 复用现有 GLSL 编译器（include path: src/render）
#include <GLFW/glfw3.h>
#include "log/LogMacros.h"
#include <iostream>
#include <vector>

namespace rhi {

static std::string shaderFileOf(const GraphicsPipelineDesc& desc, ShaderStage stage) {
    for (const auto& s : desc.shaders) {
        if (s.stage == stage) return s.path;
    }
    return "";
}

GLDevice::GLDevice(GLFWwindow* window, const std::string& shaderRootPath)
    : _window(window)
    , _shaderRootPath(shaderRootPath) {
    _commandList = std::make_unique<GLCommandList>(this);
}

GLDevice::~GLDevice() {
    waitIdle();
    for (auto& b : _buffers)   if (b.id) glDeleteBuffers(1, &b.id);
    for (auto& t : _textures)  if (t.id) glDeleteTextures(1, &t.id);
    // GLPipeline 持有 unique_ptr<Shader>，其析构负责 glDeleteProgram
}

DeviceInfo GLDevice::info() const {
    DeviceInfo di;
    di.backend     = Backend::OpenGL;
    di.backendName = "OpenGL";
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version  = glGetString(GL_VERSION);
    di.adapterName = renderer ? reinterpret_cast<const char*>(renderer) : "unknown";
    di.apiVersion  = version  ? reinterpret_cast<const char*>(version)  : "unknown";
    return di;
}

BufferHandle GLDevice::createBuffer(const BufferDesc& desc) {
    GLBuffer buf;
    buf.size  = desc.size;
    buf.usage = desc.usage;
    glGenBuffers(1, &buf.id);
    glBindBuffer(GL_ARRAY_BUFFER, buf.id);
    GLenum target = (desc.usage & BUF_INDEX) ? GL_ELEMENT_ARRAY_BUFFER : GL_ARRAY_BUFFER;
    // GL 的 target 只影响绑定槽位，数据本身与 target 无关，这里统一用 ARRAY_BUFFER 创建
    glBufferData(GL_ARRAY_BUFFER, desc.size, desc.initialData,
                 (desc.usage & BUF_CPU_WRITE) ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    (void)target;

    _buffers.push_back(std::move(buf));
    BufferHandle h; h.id = static_cast<uint32_t>(_buffers.size());
    return h;
}

TextureHandle GLDevice::createTexture(const TextureDesc& desc) {
    GLTexture tex;
    glGenTextures(1, &tex.id);
    if (desc.cubemap) {
        // 立方体贴图：6 面连续 RGBA8
        glBindTexture(GL_TEXTURE_CUBE_MAP, tex.id);
        const size_t faceBytes = static_cast<size_t>(desc.width) * desc.height * 4;
        const unsigned char* base = static_cast<const unsigned char*>(desc.initialData);
        for (int face = 0; face < 6; ++face) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, GL_RGBA8,
                         desc.width, desc.height, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                         base + face * faceBytes);
        }
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        tex.cubemap = true;

        _textures.push_back(std::move(tex));
        TextureHandle h; h.id = static_cast<uint32_t>(_textures.size());
        return h;
    }
    glBindTexture(GL_TEXTURE_2D, tex.id);
    GLenum internalFmt = (desc.format == Format::Depth32F) ? GL_DEPTH_COMPONENT32F : GL_RGBA8;
    GLenum dataFmt     = (desc.format == Format::Depth32F) ? GL_DEPTH_COMPONENT  : GL_RGBA;
    GLenum dataType    = (desc.format == Format::Depth32F) ? GL_FLOAT            : GL_UNSIGNED_BYTE;
    glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, desc.width, desc.height, 0,
                 dataFmt, dataType, desc.initialData);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    _textures.push_back(std::move(tex));
    TextureHandle h; h.id = static_cast<uint32_t>(_textures.size());
    return h;
}

PipelineHandle GLDevice::createPipeline(const GraphicsPipelineDesc& desc) {
    GLPipeline pipe;
    // 按项目着色器约定从文件构建（Shader 的析构负责删除 program）
    const std::string vertPath = _shaderRootPath + shaderFileOf(desc, ShaderStage::Vertex);
    const std::string fragPath = _shaderRootPath + shaderFileOf(desc, ShaderStage::Fragment);
    pipe.program = std::make_unique<Shader>(vertPath, fragPath);
    pipe.desc    = desc;

    _pipelines.push_back(std::move(pipe));
    PipelineHandle h; h.id = static_cast<uint32_t>(_pipelines.size());
    return h;
}

UniformBlockHandle GLDevice::createUniformBlock(uint32_t /*size*/) {
    // GL 后端：uniform 数据经 setDrawUniforms 按名直传，无独立块对象；返回哑句柄
    UniformBlockHandle h; h.id = 1;
    return h;
}

void GLDevice::updateBuffer(BufferHandle buffer, const void* data,
                            uint32_t size, uint32_t offset) {
    if (!buffer.valid() || !data) return;
    const GLBuffer& buf = this->buffer(buffer);
    if (offset + size > buf.size) {
        // 越界保护：v1 不支持扩容，超出创建时大小直接拒绝
        return;
    }
    glBindBuffer(GL_ARRAY_BUFFER, buf.id);
    glBufferSubData(GL_ARRAY_BUFFER, offset, size, data);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

// ---- v2 对象级销毁（B0）----
void GLDevice::destroyBuffer(BufferHandle h) {
    if (!h.valid() || h.id > _buffers.size()) return;
    GLBuffer& b = _buffers[h.id - 1];
    if (b.id) glDeleteBuffers(1, &b.id);
    b = GLBuffer{};
}

void GLDevice::destroyTexture(TextureHandle h) {
    if (!h.valid() || h.id > _textures.size()) return;
    GLTexture& t = _textures[h.id - 1];
    if (t.id) glDeleteTextures(1, &t.id);
    t = GLTexture{};
}

void GLDevice::destroyPipeline(PipelineHandle h) {
    if (!h.valid() || h.id > _pipelines.size()) return;
    GLPipeline& p = _pipelines[h.id - 1];
    p.program.reset();   // Shader 析构负责 glDeleteProgram
    p.desc = GraphicsPipelineDesc{};
}

void GLDevice::waitIdle() {
    glFinish();
}

IRHICommandList* GLDevice::commandList() {
    return _commandList.get();
}

uint64_t GLDevice::nativeHandle(RhiHandle handle) const {
    // 过渡期与旧 GL 代码互操作：buffer 句柄 → GLuint；texture 句柄 → GLuint
    if (handle.id >= 1 && handle.id <= _buffers.size()) {
        return _buffers[handle.id - 1].id;
    }
    if (handle.id >= 1 && handle.id <= _textures.size()) {
        return _textures[handle.id - 1].id;
    }
    return 0;
}

const GLBuffer& GLDevice::buffer(BufferHandle h) const {
    return _buffers[h.id - 1];
}

const GLTexture& GLDevice::texture(TextureHandle h) const {
    return _textures[h.id - 1];
}

const GLPipeline& GLDevice::pipeline(PipelineHandle h) const {
    return _pipelines[h.id - 1];
}

} // namespace rhi
