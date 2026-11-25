//
//  GLDevice.cpp
//
#include "GLDevice.h"
#include "GLCommandList.h"
#include "rhi/FormatInfo.h"
#include "log/LogMacros.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace rhi {

static GLenum glWrapOf(WrapMode m) {
    switch (m) {
        case WrapMode::Repeat:      return GL_REPEAT;
        case WrapMode::ClampToEdge: return GL_CLAMP_TO_EDGE;
        case WrapMode::Mirror:      return GL_MIRRORED_REPEAT;
    }
    return GL_REPEAT;
}

// B2：minFilter × mipFilter 组合成 GL 四档 mipmap 过滤（与 VK 端
// minFilter/mipmapMode 两字段一一对应；无 mip 链的纹理请勿使用 *MIPMAP* 档，
// 否则纹理按"不完整"处理采样为黑）
static GLenum glMinFilterOf(FilterMode minF, FilterMode mipF) {
    const bool nearestLevel = (minF == FilterMode::Nearest);
    const bool nearestMip   = (mipF == FilterMode::Nearest);
    if (nearestLevel && nearestMip) return GL_NEAREST_MIPMAP_NEAREST;
    if (!nearestLevel && nearestMip) return GL_LINEAR_MIPMAP_NEAREST;
    if (nearestLevel && !nearestMip) return GL_NEAREST_MIPMAP_LINEAR;
    return GL_LINEAR_MIPMAP_LINEAR;
}

// B2：按名字检查 GL 扩展。core profile 下 glGetString(GL_EXTENSIONS) 不可靠，
// 用 glGetStringi 逐个遍历（需 GL 3.0+，本后端上下文固定 3.3 起）
static bool hasGLExtension(const char* name) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; ++i) {
        const GLubyte* ext = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
        if (ext && std::strcmp(reinterpret_cast<const char*>(ext), name) == 0) {
            return true;
        }
    }
    return false;
}

static std::string shaderFileOf(const GraphicsPipelineDesc& desc, ShaderStage stage) {
    for (const auto& s : desc.shaders) {
        if (s.stage == stage) return s.path;
    }
    return "";
}

// C1a：宿主已把 GL 上下文设为 current 后调用。RHI 不创建窗口/上下文，
// 只保存尺寸与宿主交换回调。
GLDevice::GLDevice(const DeviceDesc& desc)
    : _shaderRootPath(desc.shaderRootPath)
    , _size{desc.width, desc.height}
    , _window(desc.window) {
    if (!_window.valid() || !_window.swapBuffers) {
        ENGINE_LOG_ERROR("[GL] DeviceDesc.window invalid or missing swapBuffers callback "
                         "(host owns window/swap, see docs ARCHITECTURE_INTERFACES §3.2)");
        return;   // _initialized 保持 false，工厂报错
    }
    // V3-15：MSAA——GL 的默认帧缓冲采样数由上下文创建时决定，这里只做校验。
    // 声明值与实际不一致时明确报错（不静默按实际采样数出图，避免"设置没生效"的错觉）。
    _msaaSamples = desc.msaaSamples > 1 ? desc.msaaSamples : 1;
    {
        GLint actual = 0;
        glGetIntegerv(GL_SAMPLES, &actual);
        // 请求 1 时上下文没有多采样缓冲（0）是正常情况，只在请求多采样时校验
        if (_msaaSamples > 1 && static_cast<uint32_t>(actual) != _msaaSamples) {
            ENGINE_LOG_ERROR("[GL] MSAA mismatch: requested %u, context has %d "
                             "(GL 的默认帧缓冲采样数由宿主/上下文创建决定)",
                             _msaaSamples, actual);
        }
    }
    _initialized = true;
    // V3-13：句柄表槽位复用自检（分配→释放→再分配应复用同槽、旧句柄失效）
    if (!_buffers.debugSelfCheck()) {
        ENGINE_LOG_ERROR("[GL] HandleTable reuse self-check FAILED");
    }
    _commandList = std::make_unique<GLCommandList>(this);
}

GLDevice::~GLDevice() {
    waitIdle();
    // V3-0：渲染目标的 FBO/RBO 此前析构不释放（泄漏）；颜色纹理在下方
    // _textures 循环统一删除。手动 destroyRenderTarget 过的槽位 fbo 已为 0。
    _renderTargets.forEach([&](GLRenderTarget& rt) {
        if (rt.depthRbo) glDeleteRenderbuffers(1, &rt.depthRbo);
        if (rt.fbo)      glDeleteFramebuffers(1, &rt.fbo);
    });
    _buffers.forEach([&](GLBuffer& b) { if (b.id) glDeleteBuffers(1, &b.id); });
    _textures.forEach([&](GLTexture& t) { if (t.id) glDeleteTextures(1, &t.id); });
    _samplers.forEach([&](GLSampler& s) { if (s.id) glDeleteSamplers(1, &s.id); });
    // GLPipeline 持有 unique_ptr<GLProgram>，其析构负责 glDeleteProgram
    _delQueue.flushAll();   // V3-0：延迟销毁兜底（waitIdle 后执行安全）
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

// B2：结构化设备能力，与 VK 端（VulkanDevice::capabilities + VkCaps）对齐。
// 惰性查询：调用时才 glGetIntegerv/glGetString（要求当前线程有本上下文），
// 启动时不打印——调试面板「设备能力」折叠区按需展示。
DeviceCapabilities GLDevice::capabilities() const {
    DeviceCapabilities caps;
    char buf[128];

    // 数值版本号（GL 3.0+）
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);

    // ---- limits ----
    GLint maxTexSize = 0, maxUnits = 0, maxUboRange = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSize);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &maxUnits);
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &maxUboRange);
    caps.maxTexture2D           = static_cast<uint32_t>(std::max(0, maxTexSize));
    caps.maxSampledImagesPerSet = static_cast<uint32_t>(std::max(0, maxUnits));   // 全阶段合计纹理单元
    caps.maxUniformBufferRange  = static_cast<uint32_t>(std::max(0, maxUboRange));
    // GL 无 push constant 对应物（uniform 按名直传）；GL 也查不到显存大小
    caps.maxPushConstantSize = 0;
    caps.deviceMemoryBytes   = 0;

    // ---- 特性 ----
    // VK 专属的路径开关对 GL 不适用，语义上保持 false：
    caps.dynamicRendering   = false;   // GL 没有 render pass 对象，本来就是直接渲染
    caps.synchronization2   = false;
    caps.timelineSemaphore  = false;
    caps.descriptorIndexing = false;   // 3.3 core 无 bindless texture
    caps.memoryBudget       = false;

    // 宽线（D1 CAD 边线质量）：核心上下文里 glLineWidth>1 通常受限，按实际范围判定
    GLfloat lineWidthRange[2] = {1.0f, 1.0f};
    glGetFloatv(GL_ALIASED_LINE_WIDTH_RANGE, lineWidthRange);
    caps.wideLines = lineWidthRange[1] > 1.0f;

    // 几何着色器：GL 3.2 起核心支持（macOS GL 4.1 有；MoltenVK 不支持只是 VK 侧限制）
    caps.geometryShader = (major > 3) || (major == 3 && minor >= 2);

    // 各向异性过滤：GL_EXT_texture_filter_anisotropic
    //（glad 头里 GL_MAX_TEXTURE_MAX_ANISOTROPY = 0x84FF，与 EXT 名同值）
    const bool hasAniso = hasGLExtension("GL_EXT_texture_filter_anisotropic");
    GLfloat maxAniso = 1.0f;
    if (hasAniso) {
        glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
    }
    caps.samplerAnisotropy = hasAniso && maxAniso > 1.0f;

    // sRGB 默认帧缓冲（等价 VK 的 sRGB 交换链）：GL 3.0 起核心提供
    // GL_FRAMEBUFFER_SRGB 写入编码开关（B3c 线性工作流依赖它）。
    // 注意 macOS 上 GLFW 不报告 GLFW_SRGB_CAPABLE（见 app/main.cpp B3c 注释），
    // GL_BACK 的 COLOR_ENCODING 查询也实测报 linear——按核心版本能力报告。
    caps.sRGBSwapchain = (major >= 3);

    // 诊断值：默认帧缓冲附件编码（保留 main.cpp 启动诊断的同一信息；
    // 查询前后恢复 GL_FRAMEBUFFER_BINDING，状态中性）
    GLint defaultEnc = GL_LINEAR, prevFbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // 注意：附件名必须用 GL_BACK_LEFT（GL_BACK 在本查询非法，驱动报 GL_INVALID_ENUM，
    // app/main.cpp 的启动诊断即踩过此坑——错误被忽略后 encoding 恒为 0）
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_BACK_LEFT,
                                          GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING,
                                          &defaultEnc);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(prevFbo));

    // ---- details 人类可读表（风格与 VK 端 VkCaps::toCapabilities 一致）----
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* vendor   = glGetString(GL_VENDOR);
    const GLubyte* version  = glGetString(GL_VERSION);
    const GLubyte* glsl     = glGetString(GL_SHADING_LANGUAGE_VERSION);
    auto add = [&caps](const std::string& n, const std::string& v) {
        caps.details.push_back({n, v});
    };
    auto yesNo = [](bool b) { return b ? "yes" : "no"; };
    add("设备", renderer ? reinterpret_cast<const char*>(renderer) : "unknown");
    add("厂商", vendor ? reinterpret_cast<const char*>(vendor) : "unknown");
    std::snprintf(buf, sizeof(buf), "%s (%d.%d)",
                  version ? reinterpret_cast<const char*>(version) : "unknown", major, minor);
    add("API 版本", buf);
    add("GLSL 版本", glsl ? reinterpret_cast<const char*>(glsl) : "unknown");
    std::snprintf(buf, sizeof(buf), "%u", caps.maxTexture2D);
    add("最大 2D 纹理", buf);
    std::snprintf(buf, sizeof(buf), "%u", caps.maxSampledImagesPerSet);
    add("纹理单元（combined）", buf);
    std::snprintf(buf, sizeof(buf), "%u B", caps.maxUniformBufferRange);
    add("UBO 范围上限", buf);
    add("push constants 上限", "不支持（uniform 按名直传）");
    add("显存堆", "不可查询（GL）");
    std::snprintf(buf, sizeof(buf), "%.1f..%.1f", lineWidthRange[0], lineWidthRange[1]);
    add("线宽范围", buf);
    add("wideLines", yesNo(caps.wideLines));
    add("geometryShader", yesNo(caps.geometryShader));
    std::snprintf(buf, sizeof(buf), "%s (max %.1f)", yesNo(caps.samplerAnisotropy), maxAniso);
    add("samplerAnisotropy", buf);
    add("sRGB 帧缓冲", yesNo(caps.sRGBSwapchain));
    add("默认帧缓冲编码", defaultEnc == GL_SRGB ? "sRGB" : "linear");
    return caps;
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

    RhiHandle base = _buffers.alloc(std::move(buf));
    BufferHandle h; h.id = base.id; h.gen = base.gen;
    return h;
}

TextureHandle GLDevice::createTexture(const TextureDesc& desc) {
    GLTexture tex;
    glGenTextures(1, &tex.id);

    const FormatInfo& fi = FormatInfo::of(desc.format);
    const GLenum internalFmt = static_cast<GLenum>(fi.glInternalFormat());
    const GLenum dataFmt     = static_cast<GLenum>(fi.glDataFormat());
    const GLenum dataType    = static_cast<GLenum>(fi.glDataType());

    // V3-0：上传逐字节假定紧排（R8 / 非 4 倍数宽度会错位）。GL 默认
    // GL_UNPACK_ALIGNMENT=4，只对本 Width*4 对齐安全。入口设 1，出口恢复。
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    const GLenum target = desc.cubemap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    glBindTexture(target, tex.id);
    tex.width  = desc.width;    // B4：readback/RT 需要
    tex.height = desc.height;
    tex.format = desc.format;

    if (desc.cubemap) {
        // 立方体贴图：6 面连续存放
        const uint32_t bpp = FormatInfo::of(desc.format).bytesPerPixel();
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
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);   // V3-0：恢复默认紧排假设

    RhiHandle base = _textures.alloc(std::move(tex));
    TextureHandle h; h.id = base.id; h.gen = base.gen;
    return h;
}

PipelineHandle GLDevice::createPipeline(const GraphicsPipelineDesc& desc) {
    GLPipeline pipe;
    // L0：RHI 自有 GLSL 编译器（GLProgram）。编译失败不再 assert 中止进程——
    // 返回 invalid 句柄（上层绘制自然跳过），行为改进见 ARCHITECTURE_INTERFACES §4.1
    auto prog = std::make_unique<GLProgram>();
    const std::string vertPath = _shaderRootPath + shaderFileOf(desc, ShaderStage::Vertex);
    const std::string fragPath = _shaderRootPath + shaderFileOf(desc, ShaderStage::Fragment);
    if (!prog->compile(vertPath, fragPath)) {
        ENGINE_LOG_ERROR("[GL] pipeline creation failed (shader compile); returns invalid handle");
        return {};
    }
    pipe.program = std::move(prog);
    pipe.desc    = desc;

    RhiHandle base = _pipelines.alloc(std::move(pipe));
    PipelineHandle h; h.id = base.id; h.gen = base.gen;
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

// ---- v2 独立采样器（B2）----
// 与 createTexture 的"烘焙到纹理对象"路径并存：既有纹理不受影响（参数仍烘在
// glTexParameteri），只有显式 createSampler + attachSampler 的纹理在
// bindTexture 时改走 glBindSampler（GL 采样器对象优先级高于纹理对象参数）。
SamplerHandle GLDevice::createSampler(const SamplerDesc& desc) {
    GLSampler s;
    glGenSamplers(1, &s.id);
    if (!s.id) {
        ENGINE_LOG_ERROR("[GL] createSampler: glGenSamplers failed");
        return {};
    }
    s.desc = desc;

    // 过滤：min = minFilter×mipFilter 四档组合（对应 VK minFilter+mipmapMode），
    // mag = magFilter
    glSamplerParameteri(s.id, GL_TEXTURE_MIN_FILTER, glMinFilterOf(desc.minFilter, desc.mipFilter));
    glSamplerParameteri(s.id, GL_TEXTURE_MAG_FILTER,
                        (desc.magFilter == FilterMode::Nearest) ? GL_NEAREST : GL_LINEAR);

    // 地址模式（S/T/R）。注意 GL 采样器是跨纹理复用的：立方体贴图要求
    // Clamp 类地址模式（REPEAT 采样 cube 违规），由调用方按用途设置
    //（与 VK 端 createTexture 内对 cubemap 强制 CLAMP_TO_EDGE 的差异点）。
    glSamplerParameteri(s.id, GL_TEXTURE_WRAP_S, glWrapOf(desc.wrapU));
    glSamplerParameteri(s.id, GL_TEXTURE_WRAP_T, glWrapOf(desc.wrapV));
    glSamplerParameteri(s.id, GL_TEXTURE_WRAP_R, glWrapOf(desc.wrapW));

    // LOD 区间
    glSamplerParameterf(s.id, GL_TEXTURE_MIN_LOD, desc.minLod);
    glSamplerParameterf(s.id, GL_TEXTURE_MAX_LOD, desc.maxLod);

    // 各向异性过滤：需要 GL_EXT_texture_filter_anisotropic；上限按硬件 clamp
    if (desc.maxAnisotropy > 1.0f) {
        if (hasGLExtension("GL_EXT_texture_filter_anisotropic")) {
            GLfloat maxAniso = 1.0f;
            glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY, &maxAniso);
            glSamplerParameterf(s.id, GL_TEXTURE_MAX_ANISOTROPY,
                                std::min(desc.maxAnisotropy, maxAniso));
        } else {
            ENGINE_LOG_WARN_ONCE("[GL] sampler anisotropy requested but "
                                 "GL_EXT_texture_filter_anisotropic missing");
        }
    }

    // 深度比较采样（阴影 PCF 预留）：比较函数与 VK 对齐取 GREATER
    //（Reversed-Z 时代约定，见 VkDevice.cpp compareOp 注释）
    glSamplerParameteri(s.id, GL_TEXTURE_COMPARE_MODE,
                        desc.compareEnable ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
    glSamplerParameteri(s.id, GL_TEXTURE_COMPARE_FUNC, GL_GREATER);

    RhiHandle base = _samplers.alloc(std::move(s));
    SamplerHandle h; h.id = base.id; h.gen = base.gen;
    ENGINE_LOG_INFO("[GL] sampler %u created (aniso=%.1f, compare=%d)",
                    h.id, desc.maxAnisotropy, desc.compareEnable ? 1 : 0);
    return h;
}

void GLDevice::destroySampler(SamplerHandle h) {
    if (!_samplers.alive(h)) return;
    GLSampler& s = _samplers.get(h.id);
    if (s.id) {
        glDeleteSamplers(1, &s.id);
        // 清理纹理上的悬空引用
        _textures.forEach([&](GLTexture& t) {
            if (t.sampler.id == h.id) t.sampler = SamplerHandle{};
        });
    }
    _samplers.free(h.id);
}

void GLDevice::attachSampler(TextureHandle tex, SamplerHandle s) {
    if (!_textures.alive(tex)) return;
    if (s.valid() && !_samplers.alive(s)) {
        ENGINE_LOG_WARN("[GL] attachSampler: sampler %u invalid", s.id);
        return;
    }
    GLTexture& t = _textures.get(tex.id);
    if (!t.id) return;   // 已销毁的纹理不记录
    t.sampler = s;       // s invalid = 解除关联
}

// ---- v2 对象级销毁（B0）----
// V3-0：销毁走 DeletionQueue 延迟到下一帧 beginFrame（帧在飞的资源不得立即删；
// GL 命令可能在 GPU 上仍在执行）。摘句柄立即生效（id 不复用），真删除到期批量。
void GLDevice::destroyBuffer(BufferHandle h) {
    if (!_buffers.alive(h)) return;
    GLuint id = _buffers.get(h.id).id;
    if (!id) return;
    _buffers.free(h.id);                        // 摘句柄（清 value + gen++）
    _commandList->invalidateVAOForBuffer(id);   // VAO 持属性指针，源 buffer 先摘引用
    _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                   [id] { glDeleteBuffers(1, &id); });
}

void GLDevice::destroyTexture(TextureHandle h) {
    if (!_textures.alive(h)) return;
    GLuint id = _textures.get(h.id).id;
    if (!id) return;
    _textures.free(h.id);
    _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                   [id] { glDeleteTextures(1, &id); });
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
    if (desc.hasColor) {
        TextureDesc td;
        td.width  = desc.width;
        td.height = desc.height;
        td.format = desc.colorFormat;
        td.usage  = TEX_SAMPLED | TEX_COLOR_ATTACHMENT;
        td.mipLevels = desc.mipLevels;
        td.sampler.wrapU = td.sampler.wrapV = td.sampler.wrapW = WrapMode::ClampToEdge;
        rt.colorTex = createTexture(td);
        if (!rt.colorTex.valid()) return {};
    }

    glGenFramebuffers(1, &rt.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, rt.fbo);
    if (rt.colorTex.valid()) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, texture(rt.colorTex).id, 0);
    }

    if (desc.hasDepth) {
        if (!desc.hasColor) {
            // V3-17：纯深度目标（阴影贴图）——深度用纹理，事后可采样
            TextureDesc dd;
            dd.width  = desc.width;
            dd.height = desc.height;
            dd.format = desc.depthFormat;
            dd.usage  = TEX_DEPTH_ATTACHMENT | TEX_SAMPLED;
            dd.sampler.minFilter = FilterMode::Nearest;
            dd.sampler.magFilter = FilterMode::Nearest;
            dd.sampler.wrapU = dd.sampler.wrapV = dd.sampler.wrapW = WrapMode::ClampToEdge;
            rt.depthTex = createTexture(dd);
            if (!rt.depthTex.valid()) {
                glBindFramebuffer(GL_FRAMEBUFFER, 0);
                glDeleteFramebuffers(1, &rt.fbo);
                return {};
            }
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                   GL_TEXTURE_2D, texture(rt.depthTex).id, 0);
        } else {
            GLenum depthInternal = GL_DEPTH_COMPONENT32F;
            if (desc.depthFormat == Format::D24S8) depthInternal = GL_DEPTH24_STENCIL8;
            glGenRenderbuffers(1, &rt.depthRbo);
            glBindRenderbuffer(GL_RENDERBUFFER, rt.depthRbo);
            glRenderbufferStorage(GL_RENDERBUFFER, depthInternal, desc.width, desc.height);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                      GL_RENDERBUFFER, rt.depthRbo);
        }
    }
    if (!rt.colorTex.valid()) {
        // 纯深度 FBO：不写颜色
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    }

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        ENGINE_LOG_ERROR("[GL] createRenderTarget: incomplete FBO (0x%x)", status);
        if (rt.colorTex.valid()) destroyTexture(rt.colorTex);
        if (rt.depthTex.valid()) destroyTexture(rt.depthTex);
        if (rt.depthRbo) glDeleteRenderbuffers(1, &rt.depthRbo);
        glDeleteFramebuffers(1, &rt.fbo);
        return {};
    }

    const GLuint fbo = rt.fbo;
    RhiHandle base = _renderTargets.alloc(std::move(rt));
    RenderTargetHandle h; h.id = base.id; h.gen = base.gen;
    ENGINE_LOG_INFO("[GL] render target %ux%u created (id=%u, fbo=%u, depth=%d)",
                    desc.width, desc.height, h.id, fbo, desc.hasDepth ? 1 : 0);
    return h;
}

void GLDevice::destroyRenderTarget(RenderTargetHandle h) {
    if (!_renderTargets.alive(h)) return;
    GLRenderTarget& rt = _renderTargets.get(h.id);
    if (rt.fbo) {
        if (rt.colorTex.valid()) destroyTexture(rt.colorTex);
        if (rt.depthTex.valid()) destroyTexture(rt.depthTex);
        GLuint fbo = rt.fbo, rbo = rt.depthRbo;
        // FBO/RBO 同走延迟队列（Pass 可能仍在帧内引用）
        _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames, [fbo, rbo] {
            if (rbo) glDeleteRenderbuffers(1, &rbo);
            glDeleteFramebuffers(1, &fbo);
        });
    }
    _renderTargets.free(h.id);   // 标记 dead + gen++（槽位可复用）
}

TextureHandle GLDevice::renderTargetDepthTexture(RenderTargetHandle h) const {
    if (!_renderTargets.alive(h)) return {};
    return _renderTargets.get(h.id).depthTex;
}

TextureHandle GLDevice::renderTargetColorTexture(RenderTargetHandle h) const {
    if (!_renderTargets.alive(h)) return {};
    return _renderTargets.get(h.id).colorTex;
}

// B4：同步回读。glGetTexImage 直接把整级读出（mipLevel 参数原生支持），
// 再裁出区域；GL 是即时语义，无需额外同步。
bool GLDevice::readbackTexture(TextureHandle tex, void* outData, size_t outSize,
                               const ReadbackDesc& rb) {
    if (!_textures.alive(tex) || !outData) return false;
    const GLTexture& t = _textures.get(tex.id);

    const uint32_t bpp = FormatInfo::of(t.format).bytesPerPixel();
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

    const FormatInfo& fi = FormatInfo::of(t.format);
    const GLenum internalFmt = static_cast<GLenum>(fi.glInternalFormat());
    const GLenum dataFmt     = static_cast<GLenum>(fi.glDataFormat());
    const GLenum dataType    = static_cast<GLenum>(fi.glDataType());

    // core profile 无 glGetTexImage（compat-only）：临时 FBO 挂接纹理后
    // glReadPixels 直接区域回读（顺带省掉整图缓冲）
    GLuint fbo = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, t.id, static_cast<int>(rb.mipLevel));
    const GLenum status = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);
        ENGINE_LOG_WARN("[GL] readback: incomplete read FBO (0x%x)", status);
        return false;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(rb.x, rb.y, w, h, dataFmt, dataType, outData);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo);
    return true;
}

void GLDevice::destroyPipeline(PipelineHandle h) {
    if (!_pipelines.alive(h)) return;
    if (_pipelines.get(h.id).program) {
        // V3-0：先清 VAO 缓存（VAO 固化了管线的属性布局指针），再删 program。
        // glDeleteProgram 自带"在用即标记删除"语义（帧在飞安全），无需入队。
        _commandList->invalidateVAOForPipeline(h.id);
    }
    _pipelines.free(h.id);   // 重置 unique_ptr<GLProgram>（glDeleteProgram）+ desc
}

void GLDevice::waitIdle() {
    glFinish();
}

// V3-0：帧号推进 + 到期延迟销毁。GL 路径 RenderSystem 每帧调用一次；
// 遗留叠加路径（drawSmokeTriangle 等）的额外调用幂等（flush 空队列无副作用）。
bool GLDevice::beginFrame() {
    ++_frameCount;
    _delQueue.flush(_frameCount);
    return true;
}

// ---- C1a：尺寸推送 + 呈现 ----
void GLDevice::onResize(int width, int height) {
    if (width > 0 && height > 0) {
        _size.width  = static_cast<uint32_t>(width);
        _size.height = static_cast<uint32_t>(height);
    }
}

void GLDevice::present() {
    // 宿主拥有窗口与交换时机策略（vsync 等）；RHI 只转发回调
    if (_window.swapBuffers) {
        _window.swapBuffers(_window.swapUser);
    }
}

IRHICommandList* GLDevice::commandList() {
    return _commandList.get();
}

uint64_t GLDevice::nativeBuffer(BufferHandle h) const {
    return _buffers.alive(h) ? _buffers.get(h.id).id : 0;
}
uint64_t GLDevice::nativeTexture(TextureHandle h) const {
    return _textures.alive(h) ? _textures.get(h.id).id : 0;
}
uint64_t GLDevice::nativeSampler(SamplerHandle h) const {
    return _samplers.alive(h) ? _samplers.get(h.id).id : 0;
}
uint64_t GLDevice::nativePipeline(PipelineHandle h) const {
    return (_pipelines.alive(h) && _pipelines.get(h.id).program)
        ? _pipelines.get(h.id).program->id() : 0;
}

const GLBuffer& GLDevice::buffer(BufferHandle h) const {
    return _buffers.get(h.id);
}

const GLTexture& GLDevice::texture(TextureHandle h) const {
    return _textures.get(h.id);
}

const GLSampler& GLDevice::sampler(SamplerHandle h) const {
    return _samplers.get(h.id);
}

const GLPipeline& GLDevice::pipeline(PipelineHandle h) const {
    return _pipelines.get(h.id);
}

const GLRenderTarget& GLDevice::renderTarget(RenderTargetHandle h) const {
    return _renderTargets.get(h.id);
}

// V3-1：句柄有效性（id 范围 + gen 代际校验）
bool GLDevice::alive(BufferHandle h)   const { return _buffers.alive(h); }
bool GLDevice::alive(TextureHandle h)  const { return _textures.alive(h); }
bool GLDevice::alive(PipelineHandle h) const { return _pipelines.alive(h); }
bool GLDevice::alive(SamplerHandle h)  const { return _samplers.alive(h); }

} // namespace rhi
