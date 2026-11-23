//
//  GLDevice.cpp
//
#include "GLDevice.h"
#include "GLCommandList.h"
#include "shader.h"                 // 复用现有 GLSL 编译器（include path: src/render）
#include <GLFW/glfw3.h>
#include "log/LogMacros.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace rhi {

// Format → (internalFormat, dataFormat, dataType)。B3c：颜色格式用 sRGB 内部格式，
// 硬件采样时解码到线性；法线/高度/ID 等数据格式保持线性。
static void glFormatOf(Format f, GLenum& internal, GLenum& dataFmt, GLenum& dataType) {
    internal = GL_RGBA8; dataFmt = GL_RGBA; dataType = GL_UNSIGNED_BYTE;
    switch (f) {
        case Format::RGBA8_SRGB: internal = GL_SRGB8_ALPHA8; dataFmt = GL_RGBA; break;
        case Format::BGRA8_UNorm: internal = GL_RGBA8;   dataFmt = GL_BGRA; break;
        case Format::BGRA8_SRGB:  internal = GL_SRGB8_ALPHA8; dataFmt = GL_BGRA; break;
        case Format::R8_UNorm:    internal = GL_R8;      dataFmt = GL_RED; break;
        case Format::R16F:        internal = GL_R16F;    dataFmt = GL_RED;  dataType = GL_FLOAT; break;
        case Format::RGBA16F:     internal = GL_RGBA16F; dataFmt = GL_RGBA; dataType = GL_FLOAT; break;
        case Format::RGBA32F:     internal = GL_RGBA32F; dataFmt = GL_RGBA; dataType = GL_FLOAT; break;
        case Format::R32_UInt:    internal = GL_R32UI;   dataFmt = GL_RED_INTEGER; dataType = GL_UNSIGNED_INT; break;
        case Format::Depth32F:    internal = GL_DEPTH_COMPONENT32F; dataFmt = GL_DEPTH_COMPONENT; dataType = GL_FLOAT; break;
        case Format::D24S8:       internal = GL_DEPTH24_STENCIL8;   dataFmt = GL_DEPTH_STENCIL;
                                  dataType = GL_UNSIGNED_INT_24_8; break;
        case Format::RGBA8_UNorm:
        default: break;
    }
}

static GLenum glWrapOf(WrapMode m) {
    switch (m) {
        case WrapMode::Repeat:      return GL_REPEAT;
        case WrapMode::ClampToEdge: return GL_CLAMP_TO_EDGE;
        case WrapMode::Mirror:      return GL_MIRRORED_REPEAT;
    }
    return GL_REPEAT;
}

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

    GLenum internalFmt = GL_RGBA8, dataFmt = GL_RGBA, dataType = GL_UNSIGNED_BYTE;
    glFormatOf(desc.format, internalFmt, dataFmt, dataType);

    const GLenum target = desc.cubemap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    glBindTexture(target, tex.id);
    tex.width  = desc.width;    // B4：readback/RT 需要
    tex.height = desc.height;
    tex.format = desc.format;

    if (desc.cubemap) {
        // 立方体贴图：6 面连续存放
        const uint32_t bpp = formatBytesPerPixel(desc.format);
        const size_t faceBytes = static_cast<size_t>(desc.width) * desc.height * bpp;
        const unsigned char* base = static_cast<const unsigned char*>(desc.initialData);
        for (int face = 0; face < 6; ++face) {
            glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + face, 0, internalFmt,
                         desc.width, desc.height, 0, dataFmt, dataType,
                         base ? base + face * faceBytes : nullptr);
        }
        tex.cubemap = true;
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, internalFmt, desc.width, desc.height, 0,
                     dataFmt, dataType, desc.initialData);
    }

    // B3c：mipmap（generateMips / mipLevels==0 语义与 VK 对齐）
    const bool wantMips = desc.generateMips || desc.mipLevels == 0;
    if (wantMips) {
        glGenerateMipmap(target);
    }

    // B3：采样参数由 TextureDesc::sampler 驱动（GL 侧在纹理对象上烘焙）
    const SamplerDesc& sd = desc.sampler;
    GLenum minFilter;
    if (wantMips) {
        minFilter = (sd.mipFilter == FilterMode::Nearest) ? GL_NEAREST_MIPMAP_NEAREST
                                                          : GL_LINEAR_MIPMAP_LINEAR;
    } else {
        minFilter = (sd.minFilter == FilterMode::Nearest) ? GL_NEAREST : GL_LINEAR;
    }
    const GLenum magFilter = (sd.magFilter == FilterMode::Nearest) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(target, GL_TEXTURE_MIN_FILTER, minFilter);
    glTexParameteri(target, GL_TEXTURE_MAG_FILTER, magFilter);
    glTexParameteri(target, GL_TEXTURE_WRAP_S, glWrapOf(sd.wrapU));
    glTexParameteri(target, GL_TEXTURE_WRAP_T, glWrapOf(sd.wrapV));
    if (desc.cubemap) {
        // GL 与 VK 一致：立方体采样禁用 REPEAT（天空盒用 ClampToEdge）
        glTexParameteri(target, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    } else {
        glTexParameteri(target, GL_TEXTURE_WRAP_R, glWrapOf(sd.wrapW));
    }
    glTexParameterf(target, GL_TEXTURE_MIN_LOD, sd.minLod);
    glTexParameterf(target, GL_TEXTURE_MAX_LOD, sd.maxLod);
    glBindTexture(target, 0);

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

// ---- B4：离屏渲染目标 ----
RenderTargetHandle GLDevice::createRenderTarget(const RenderTargetDesc& desc) {
    if (desc.width == 0 || desc.height == 0) {
        ENGINE_LOG_ERROR("[GL] createRenderTarget: zero-sized target");
        return {};
    }
    GLRenderTarget rt;
    rt.width  = desc.width;
    rt.height = desc.height;

    // 颜色纹理：复用 createTexture（可采样、可回读；GL 下附件绑定到 FBO）
    TextureDesc td;
    td.width  = desc.width;
    td.height = desc.height;
    td.format = desc.colorFormat;
    td.usage  = TEX_SAMPLED | TEX_COLOR_ATTACHMENT;
    td.mipLevels = desc.mipLevels;
    td.sampler.wrapU = td.sampler.wrapV = td.sampler.wrapW = WrapMode::ClampToEdge;
    rt.colorTex = createTexture(td);
    if (!rt.colorTex.valid()) return {};

    glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, texture(rt.colorTex).id, 0);

    if (desc.hasDepth) {
        GLenum depthInternal = GL_DEPTH_COMPONENT32F;
        if (desc.depthFormat == Format::D24S8) depthInternal = GL_DEPTH24_STENCIL8;
        glGenRenderbuffers(1, &rt.depthRbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rt.depthRbo);
        glRenderbufferStorage(GL_RENDERBUFFER, depthInternal, desc.width, desc.height);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                  GL_RENDERBUFFER, rt.depthRbo);
    }

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        ENGINE_LOG_ERROR("[GL] createRenderTarget: incomplete FBO (0x%x)", status);
        destroyTexture(rt.colorTex);
        if (rt.depthRbo) glDeleteRenderbuffers(1, &rt.depthRbo);
        glDeleteFramebuffers(1, &rt.fbo);
        return {};
    }

    _renderTargets.push_back(rt);
    RenderTargetHandle h; h.id = static_cast<uint32_t>(_renderTargets.size());
    ENGINE_LOG_INFO("[GL] render target %ux%u created (id=%u, fbo=%u, depth=%d)",
                    desc.width, desc.height, h.id, rt.fbo, desc.hasDepth ? 1 : 0);
    return h;
}

void GLDevice::destroyRenderTarget(RenderTargetHandle h) {
    if (!h.valid() || h.id > _renderTargets.size()) return;
    GLRenderTarget& rt = _renderTargets[h.id - 1];
    if (rt.fbo) {
        destroyTexture(rt.colorTex);
        if (rt.depthRbo) glDeleteRenderbuffers(1, &rt.depthRbo);
        glDeleteFramebuffers(1, &rt.fbo);
        rt = GLRenderTarget{};
    }
    rt.alive = false;   // 槽位保留（id 不复用）
}

TextureHandle GLDevice::renderTargetColorTexture(RenderTargetHandle h) const {
    if (!h.valid() || h.id > _renderTargets.size()) return {};
    return _renderTargets[h.id - 1].colorTex;
}

// B4：同步回读。glGetTexImage 直接把整级读出（mipLevel 参数原生支持），
// 再裁出区域；GL 是即时语义，无需额外同步。
bool GLDevice::readbackTexture(TextureHandle tex, void* outData, size_t outSize,
                               const ReadbackDesc& rb) {
    if (!tex.valid() || tex.id > _textures.size() || !outData) return false;
    const GLTexture& t = _textures[tex.id - 1];
    if (!t.id) return false;

    const uint32_t bpp = formatBytesPerPixel(t.format);
    if (bpp == 0) return false;
    const uint32_t mipW = std::max(1u, t.width  >> rb.mipLevel);
    const uint32_t mipH = std::max(1u, t.height >> rb.mipLevel);
    const uint32_t w = rb.width  ? std::min(rb.width,  mipW) : mipW;
    const uint32_t h = rb.height ? std::min(rb.height, mipH) : mipH;
    const size_t bytes = static_cast<size_t>(w) * h * bpp;
    if (outSize < bytes) {
        ENGINE_LOG_WARN("[GL] readback: outSize %zu < %zu bytes", outSize, bytes);
        return false;
    }
    if (t.cubemap || rb.arrayLayer != 0) {
        ENGINE_LOG_WARN_ONCE("[GL] readback: cubemap/array readback not supported yet");
        return false;
    }

    GLenum internalFmt = GL_RGBA8, dataFmt = GL_RGBA, dataType = GL_UNSIGNED_BYTE;
    glFormatOf(t.format, internalFmt, dataFmt, dataType);

    std::vector<unsigned char> full(static_cast<size_t>(mipW) * mipH * bpp);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, t.id);
    glGetTexImage(GL_TEXTURE_2D, static_cast<int>(rb.mipLevel), dataFmt, dataType, full.data());
    glBindTexture(GL_TEXTURE_2D, 0);

    for (uint32_t y = 0; y < h; ++y) {
        std::memcpy(static_cast<unsigned char*>(outData) + static_cast<size_t>(y) * w * bpp,
                    full.data() + (static_cast<size_t>(y) * mipW + rb.x) * bpp,
                    static_cast<size_t>(w) * bpp);
    }
    return true;
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

const GLRenderTarget& GLDevice::renderTarget(RenderTargetHandle h) const {
    return _renderTargets[h.id - 1];
}

} // namespace rhi
