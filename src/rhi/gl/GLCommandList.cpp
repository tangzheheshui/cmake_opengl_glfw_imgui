//
//  GLCommandList.cpp
//
#include "GLCommandList.h"
#include "GLDevice.h"
#include "log/LogMacros.h"
// L0：uniform 语义由 rhi/gl/GLProgram 承载；DrawUniforms ABI 迁移按
// docs/ARCHITECTURE_INTERFACES.md 2026-11-24 状态对齐暂缓（V3-2 绑定层重做时处理），
// 是 src/rhi → render 的唯一残留 include
#include "render/DrawUniforms.h"

namespace rhi {

static GLenum mapPrimitive(PrimitiveType p) {
    switch (p) {
        case PrimitiveType::Triangles:      return GL_TRIANGLES;
        case PrimitiveType::Lines:          return GL_LINES;
        case PrimitiveType::TriangleStrip:  return GL_TRIANGLE_STRIP;
    }
    return GL_TRIANGLES;
}

GLCommandList::GLCommandList(GLDevice* device)
    : _device(device) {
}

GLCommandList::~GLCommandList() {
    for (auto& [key, vao] : _vaoCache) {
        if (vao) glDeleteVertexArrays(1, &vao);
    }
}

void GLCommandList::saveState(GLStateSnapshot& s) {
    s.depthTest = glIsEnabled(GL_DEPTH_TEST);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
    s.blend     = glIsEnabled(GL_BLEND);
    s.cullFace  = glIsEnabled(GL_CULL_FACE);
    glGetIntegerv(GL_CULL_FACE_MODE, &s.cullMode);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &s.framebuffer);   // D2: RT pass 后恢复 FBO
    glGetIntegerv(GL_VIEWPORT, s.viewport);                  // C2: 视口/裁剪纳入 Pass 边界
    s.scissorTest = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_SCISSOR_BOX, s.scissorBox);
    glGetIntegerv(GL_POLYGON_MODE, s.polygonMode);           // D5: 多边形模式纳入 Pass 边界
    s.polyOffsetLine = glIsEnabled(GL_POLYGON_OFFSET_LINE);  // D5: 线偏移纳入 Pass 边界
    s.srgbWrite     = glIsEnabled(GL_FRAMEBUFFER_SRGB);      // Z: sRGB 编码纳入 Pass 边界
}

void GLCommandList::restoreState(const GLStateSnapshot& s) {
    if (s.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(s.depthMask);
    if (s.blend)     glEnable(GL_BLEND);     else glDisable(GL_BLEND);
    // D2：FBO 绑定纳入 Pass 边界——RT pass 后恢复，否则宿主后续渲染
    //（如 ImGui overlay）会画进离屏 RT
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(s.framebuffer));
    // C2：视口/裁剪恢复（Pass 不向周边泄漏多视口状态）
    glViewport(s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]);
    glScissor(s.scissorBox[0], s.scissorBox[1], s.scissorBox[2], s.scissorBox[3]);
    if (s.scissorTest) glEnable(GL_SCISSOR_TEST); else glDisable(GL_SCISSOR_TEST);
    if (s.cullFace) {
        glEnable(GL_CULL_FACE);
        glCullFace(s.cullMode);
    } else {
        glDisable(GL_CULL_FACE);
    }
    // D5：多边形模式/线偏移恢复（Pass 不向遗留 GL 代码泄漏线框状态）
    glPolygonMode(GL_FRONT_AND_BACK, static_cast<GLenum>(s.polygonMode[0]));
    if (s.polyOffsetLine) glEnable(GL_POLYGON_OFFSET_LINE); else glDisable(GL_POLYGON_OFFSET_LINE);
    glPolygonOffset(0.0f, 0.0f);
    if (s.srgbWrite) glEnable(GL_FRAMEBUFFER_SRGB); else glDisable(GL_FRAMEBUFFER_SRGB);
}

void GLCommandList::beginRenderPass(const RenderPassDesc& desc) {
    // 状态快照：保证 Pass 对周边遗留 GL 代码状态中性（见 saveState 注释）
    saveState(_savedState);
    _stateSaved = true;

    // B2：采样器槽位跟踪表清零（只记录本 Pass 内的绑定，见 endRenderPass）
    for (uint32_t i = 0; i < 32; ++i) _samplerSlotsTouched[i] = false;

    // Z（去后端化）：sRGB 颜色编码是 GL 的全局状态，VK 由附件格式驱动。
    // Pass 期间默认打开（场景颜色写回按 sRGB 编码，与 VK 交换链一致）；
    // 宿主 UI 在 Pass 内通过 setSrgbWrite(false) 关掉，保持既有字节外观。
    glEnable(GL_FRAMEBUFFER_SRGB);

    // V3-12：记录本 Pass 目标（bindPipeline 用来比对附件格式）
    _curRenderTarget = desc.renderTarget;
    _inPass          = true;
    _formatMismatch  = false;
    // V3-15：主帧缓冲采样数来自设备；离屏 RT 暂不支持 MSAA（恒 1）
    _passSamples     = desc.renderTarget.valid()
                           ? _device->renderTarget(desc.renderTarget).samples
                           : _device->msaaSamples();

    // B4：离屏 RenderTarget（RHI 资源 FBO）优先于 v1 遗留 FBO 直通
    _depthOnlyPass = desc.renderTarget.valid()
                         && !_device->renderTarget(desc.renderTarget).colorTex.valid();
    if (desc.renderTarget.valid()) {
        const GLRenderTarget& rt = _device->renderTarget(desc.renderTarget);
        glBindFramebuffer(GL_FRAMEBUFFER, rt.alive ? rt.fbo : 0);
    } else {
        // 没有离屏目标 = 默认帧缓冲（屏幕）。v1 的 GL FBO 名直通已删除：
        // 阴影等 GL Pass 改走 RenderTargetHandle，契约层不再出现后端对象名。
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    if (desc.clearColor) {
        glClearColor(desc.clearColorValue[0], desc.clearColorValue[1],
                     desc.clearColorValue[2], desc.clearColorValue[3]);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    if (desc.clearDepth) {
        // 纯深度 Pass 走 Reversed-Z（清 0 = 最远，与 VK 一致）；普通 Pass 仍用
        // GL 标准约定（desc.depthClearValue 默认 1 = 最远）。
        glClearDepth(_depthOnlyPass ? 0.0f : desc.depthClearValue);
        glClear(GL_DEPTH_BUFFER_BIT);
    }

    // 视口：desc 未指定时取 RT 尺寸（B4）或设备保存的帧缓冲尺寸
    //（C1a：RHI 不回查窗口系统；Retina 下 framebuffer 尺寸 ≠ 窗口逻辑尺寸）
    int w = static_cast<int>(desc.viewportWidth);
    int h = static_cast<int>(desc.viewportHeight);
    if (w <= 0 || h <= 0) {
        if (desc.renderTarget.valid()) {
            const GLRenderTarget& rt = _device->renderTarget(desc.renderTarget);
            w = static_cast<int>(rt.width);
            h = static_cast<int>(rt.height);
        } else {
            const auto fb = _device->framebufferSize();
            w = static_cast<int>(fb.width);
            h = static_cast<int>(fb.height);
        }
    }
    glViewport(0, 0, w, h);
    _curTargetHeight = h;   // C2: scissor 坐标换算基准
    _vpW = static_cast<uint32_t>(w);
    _vpH = static_cast<uint32_t>(h);
}

// C2：裁剪矩形。契约坐标 = 当前 pass 目标的左上原点（与窗口/鼠标一致），
// GL 帧缓冲原点在左下 → y 翻转换算。
void GLCommandList::setScissor(int x, int y, int w, int h) {
    const int targetH = _curTargetHeight > 0 ? _curTargetHeight : 0;
    const int yGL = targetH - y - h;
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, yGL, w, h);
}

void GLCommandList::endRenderPass() {
    // V3-22：MSAA 离屏目标——Pass 结束 blit 到单采样 resolve 纹理
    if (_inPass && _curRenderTarget.valid()) {
        const GLRenderTarget& rt = _device->renderTarget(_curRenderTarget);
        if (rt.multisample && rt.resolveFbo) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, rt.fbo);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, rt.resolveFbo);
            glBlitFramebuffer(0, 0, static_cast<GLint>(rt.width), static_cast<GLint>(rt.height),
                              0, 0, static_cast<GLint>(rt.width), static_cast<GLint>(rt.height),
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
        }
    }
    // B2：把本 Pass 内绑定过独立采样器的单元解绑（GL_SAMPLER_BINDING 也是
    // 全局状态），采样器不泄漏给 Pass 外的遗留 GL 代码——纹理对象上烘焙的
    // glTexParameteri 参数随即照常生效
    if (_device->hasSamplers()) {
        for (uint32_t i = 0; i < 32; ++i) {
            if (_samplerSlotsTouched[i]) {
                glBindSampler(static_cast<GLuint>(i), 0);
                _samplerSlotsTouched[i] = false;
            }
        }
    }
    if (_stateSaved) {
        restoreState(_savedState);
        _stateSaved = false;
    }
    _inPass          = false;
    _formatMismatch  = false;
    _curRenderTarget = RenderTargetHandle{};
}

void GLCommandList::bindPipeline(PipelineHandle pipeline) {
    if (!pipeline.valid()) return;
    _curPipeline = pipeline;

    const GraphicsPipelineDesc& desc = _device->pipeline(pipeline).desc;

    // V3-12：GL 的 FBO 附件格式由纹理决定，管线声明的格式必须与实际附件一致。
    // 默认帧缓冲（无离屏目标）不做格式比对——GL 没有可查询的统一格式。
    _formatMismatch = false;
    if (_inPass && _curRenderTarget.valid()) {
        const GLRenderTarget& rt = _device->renderTarget(_curRenderTarget);
        // V3-17：纯深度 FBO 只能接纯深度管线；带颜色 FBO 要求格式一致
        if (!rt.colorTex.valid()) {
            if (desc.attachment.colorCount != 0) {
                _formatMismatch = true;
                ENGINE_LOG_ERROR("[GL] pass is depth-only but pipeline writes color — draws skipped");
            }
        } else if (desc.attachment.colorCount == 0) {
            _formatMismatch = true;
            ENGINE_LOG_ERROR("[GL] pass has color but pipeline is depth-only — draws skipped");
        } else {
            const GLTexture& colorTex = _device->texture(rt.colorTex);
            if (colorTex.format != desc.attachment.colorFormats[0]) {
                _formatMismatch = true;
                ENGINE_LOG_ERROR("[GL] pass attachment mismatch: pipeline colorFormat=%d "
                                 "vs FBO %d — draws skipped",
                                 static_cast<int>(desc.attachment.colorFormats[0]),
                                 static_cast<int>(colorTex.format));
            }
        }
    }
    // MSAA：管线声明的采样数必须与 Pass 目标一致
    if (desc.attachment.samples != _passSamples) {
        _formatMismatch = true;
        ENGINE_LOG_ERROR("[GL] pass samples mismatch: pipeline=%u pass=%u — draws skipped",
                         desc.attachment.samples, _passSamples);
    }

    // 深度状态
    if (desc.depth.depthTest) glEnable(GL_DEPTH_TEST); else glDisable(GL_DEPTH_TEST);
    glDepthMask(desc.depth.depthWrite ? GL_TRUE : GL_FALSE);
    if (_depthOnlyPass) {
        // 纯深度 Pass：Reversed-Z（1=近/0=远），清 0 后 GREATER 保留最近表面
        glDepthFunc(desc.depth.depthLessEqual ? GL_GEQUAL : GL_GREATER);
    } else {
        glDepthFunc(desc.depth.depthLessEqual ? GL_LEQUAL : GL_LESS);
    }

    // 裁剪
    if (desc.cull == CullMode::Back) {
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    } else {
        glDisable(GL_CULL_FACE);
    }

    // 混合
    if (desc.blend.blendEnable) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

    // D5：多边形模式 + 边线深度偏移（线框显示模式）
    glPolygonMode(GL_FRONT_AND_BACK,
                  desc.raster.polygonMode == PolygonMode::Line ? GL_LINE : GL_FILL);
    if (desc.raster.depthBiasConstantFactor != 0.0f || desc.raster.depthBiasSlopeFactor != 0.0f) {
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(desc.raster.depthBiasSlopeFactor, desc.raster.depthBiasConstantFactor);
    } else {
        glDisable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(0.0f, 0.0f);
    }
}

void GLCommandList::setUniform(const char* name, const UniformValue& value) {
    _pendingUniforms.emplace_back(name, value);
}

void GLCommandList::bindVertexBuffer(BufferHandle buffer, uint32_t binding) {
    if (binding == 0) {
        _curVertexBuffer = buffer;
    } else {
        _curInstanceBuffer = buffer;   // 实例率数据流（obtainVAO 配置实例属性时使用）
    }
}

void GLCommandList::bindIndexBuffer(BufferHandle buffer) {
    _curIndexBuffer = buffer;
}

void GLCommandList::bindTexture(TextureHandle texture, uint32_t slot, const char* samplerName) {
    if (!texture.valid() || slot >= 32) return;
    // 注意：不能用 nativeHandle（buffer/texture 的 GL id 空间独立，
    // 旧实现会把纹理句柄误判成缓冲句柄，绑定失败导致采样全黑）
    const GLTexture& t = _device->texture(texture);
    const GLenum target = t.cubemap ? GL_TEXTURE_CUBE_MAP : GL_TEXTURE_2D;
    glActiveTexture(GL_TEXTURE0 + slot);
    glBindTexture(target, t.id);
    glActiveTexture(GL_TEXTURE0);

    // B2：显式关联的独立采样器优先于纹理对象烘焙参数（GL 采样器对象语义：
    // 绑到单元后覆盖该单元纹理的 glTexParameteri）。
    // 设备从未创建独立采样器时完全不触碰 GL_SAMPLER_BINDING，遗留路径零状态
    // 变化；一旦用了采样器，每次绑定都显式写单元（绑 0 = 用烘焙参数），避免
    // 上一张纹理的采样器泄漏到下一张。
    if (_device->hasSamplers()) {
        const GLuint sid = t.sampler.valid() ? _device->sampler(t.sampler).id : 0;
        glBindSampler(slot, sid);
        if (sid) _samplerSlotsTouched[slot] = true;
    }

    // GL 没有描述符集：采样器 uniform 必须显式指向纹理单元。
    // 业务侧只需绑定一次（VK 忽略名字，set 索引就是槽位）。
    if (samplerName && _curPipeline.valid()) {
        const GLPipeline& pipe = _device->pipeline(_curPipeline);
        if (pipe.program) {
            pipe.program->use();
            pipe.program->setInt(samplerName, static_cast<int>(slot));
        }
    }
}

void GLCommandList::setViewport(float x, float y, float w, float h) {
    // 契约坐标 = 目标左上原点。VK 侧负高度 viewport（y = tile 底 + 负高）
    // 在 GL（原点左下）换算：glViewport y = 目标高 - y，高度取绝对值。
    if (h < 0.0f) {
        const int targetH = _curTargetHeight > 0 ? _curTargetHeight : 0;
        glViewport(static_cast<GLint>(x), targetH - static_cast<GLint>(y),
                   static_cast<GLsizei>(w), static_cast<GLsizei>(-h));
    } else {
        glViewport(static_cast<GLint>(x), static_cast<GLint>(y),
                   static_cast<GLsizei>(w), static_cast<GLsizei>(h));
    }
    _vpW = static_cast<uint32_t>(w < 0 ? -w : w);
    _vpH = static_cast<uint32_t>(h < 0 ? -h : h);
}

void GLCommandList::viewportSize(uint32_t& w, uint32_t& h) const {
    w = _vpW;
    h = _vpH;
}

void GLCommandList::setSrgbWrite(bool enabled) {
    if (enabled) glEnable(GL_FRAMEBUFFER_SRGB);
    else         glDisable(GL_FRAMEBUFFER_SRGB);
}

// V3-0：销毁后清理 VAO 缓存。VAO 固化的是 GL 对象名——源资源删除后驱动
// 可能复用同名 id 给新资源，旧 VAO 就会指向错误对象（画出垃圾/踩内存）。
void GLCommandList::invalidateVAOForBuffer(uint32_t bufferId) {
    for (auto it = _vaoCache.begin(); it != _vaoCache.end();) {
        const auto& key = it->first;
        if (std::get<1>(key) == bufferId || std::get<2>(key) == bufferId) {
            if (it->second) glDeleteVertexArrays(1, &it->second);
            it = _vaoCache.erase(it);
        } else {
            ++it;
        }
    }
}

void GLCommandList::invalidateVAOForPipeline(uint32_t pipelineId) {
    for (auto it = _vaoCache.begin(); it != _vaoCache.end();) {
        if (std::get<0>(it->first) == pipelineId) {
            if (it->second) glDeleteVertexArrays(1, &it->second);
            it = _vaoCache.erase(it);
        } else {
            ++it;
        }
    }
}

GLuint GLCommandList::obtainVAO() {
    if (!_curPipeline.valid() || !_curVertexBuffer.valid()) return 0;

    const auto key = std::make_tuple(_curPipeline.id, _curVertexBuffer.id, _curInstanceBuffer.id);
    auto it = _vaoCache.find(key);
    if (it != _vaoCache.end()) {
        return it->second;
    }

    // 未命中：创建并按管线布局配置
    const GLPipeline& pipe = _device->pipeline(_curPipeline);
    const VertexLayoutDesc& layout = pipe.desc.vertexLayout;
    const GLBuffer& vb = _device->buffer(_curVertexBuffer);

    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vb.id);

    const GLBuffer* instanceBuf = nullptr;
    if (_curInstanceBuffer.valid()) {
        instanceBuf = &_device->buffer(_curInstanceBuffer);
    }
    for (const auto& attr : layout.attributes) {
        glEnableVertexAttribArray(attr.location);
        GLint size = 1;
        switch (attr.format) {
            case VertexFormat::Float1: size = 1; break;
            case VertexFormat::Float2: size = 2; break;
            case VertexFormat::Float3: size = 3; break;
            case VertexFormat::Float4: size = 4; break;
        }
        if (attr.instanced) {
            // 实例化属性从实例流缓冲读取（binding1）
            if (instanceBuf) {
                glBindBuffer(GL_ARRAY_BUFFER, instanceBuf->id);
                glVertexAttribPointer(attr.location, size, GL_FLOAT, GL_FALSE,
                                      layout.instanceStride,
                                      reinterpret_cast<const void*>(static_cast<uintptr_t>(attr.offset)));
                glVertexAttribDivisor(attr.location, 1);
            }
        } else {
            glBindBuffer(GL_ARRAY_BUFFER, vb.id);
            glVertexAttribPointer(attr.location, size, GL_FLOAT, GL_FALSE,
                                  layout.stride,
                                  reinterpret_cast<const void*>(static_cast<uintptr_t>(attr.offset)));
            glVertexAttribDivisor(attr.location, 0);
        }
    }
    glBindVertexArray(0);

    _vaoCache[key] = vao;
    return vao;
}

void GLCommandList::flushUniforms() {
    if (_pendingUniforms.empty() || !_curPipeline.valid()) {
        _pendingUniforms.clear();
        return;
    }
    const GLPipeline& pipe = _device->pipeline(_curPipeline);
    pipe.program->use();   // GL 要求先 use 再设 uniform

    for (auto& [name, u] : _pendingUniforms) {
        const char* n = name.c_str();
        switch (u.type) {
            case UniformValue::Type::Float:
                pipe.program->setFloat(n, u.data[0]);
                break;
            case UniformValue::Type::Vec3:
                pipe.program->setFloat3(n, u.data[0], u.data[1], u.data[2]);
                break;
            case UniformValue::Type::Vec4:
                pipe.program->setFloat4(n, u.data[0], u.data[1], u.data[2], u.data[3]);
                break;
            case UniformValue::Type::Mat4:
                pipe.program->setMat4(n, u.data);   // L0：直接传 float[16]
                break;
            case UniformValue::Type::Int:
                pipe.program->setInt(n, static_cast<int>(u.data[0]));
                break;
        }
    }
    _pendingUniforms.clear();
}

void GLCommandList::setDrawUniforms(UniformBlockHandle block, const void* data, uint32_t size) {
    (void)block;   // GL 按名直传，block 为哑句柄
    // 按引擎约定字段名拆解为 glUniform*（布局约定见 src/render/DrawUniforms.h）
    if (!data || size < sizeof(engine::DrawUniforms) || !_curPipeline.valid()) return;
    const auto* u = static_cast<const engine::DrawUniforms*>(data);
    const GLPipeline& pipe = _device->pipeline(_curPipeline);
    pipe.program->use();
    pipe.program->setFloat4("uLight.position", u->lightPosition_isOpen.x, u->lightPosition_isOpen.y, u->lightPosition_isOpen.z, u->lightPosition_isOpen.w);
    pipe.program->setBool("uLight.is_open", u->lightPosition_isOpen.w > 0.5f);
    pipe.program->setFloat3("uLight.direction", u->lightDirection_cosTheta.x, u->lightDirection_cosTheta.y, u->lightDirection_cosTheta.z);
    pipe.program->setFloat("uLight.cosTheta", u->lightDirection_cosTheta.w);
    pipe.program->setFloat3("uLight.ambient", u->lightAmbient.x, u->lightAmbient.y, u->lightAmbient.z);
    pipe.program->setFloat3("uLight.diffuse", u->lightDiffuse.x, u->lightDiffuse.y, u->lightDiffuse.z);
    pipe.program->setFloat3("uLight.specular", u->lightSpecular.x, u->lightSpecular.y, u->lightSpecular.z);
    pipe.program->setFloat3("uCameraPos", u->cameraPos.x, u->cameraPos.y, u->cameraPos.z);
    pipe.program->setFloat3("uMaterail.ambient", u->materialAmbient.x, u->materialAmbient.y, u->materialAmbient.z);
    pipe.program->setFloat3("uMaterail.diffuse", u->materialDiffuse.x, u->materialDiffuse.y, u->materialDiffuse.z);
    pipe.program->setFloat3("uMaterail.specular", u->materialSpecular.x, u->materialSpecular.y, u->materialSpecular.z);
    pipe.program->setFloat("uMaterail.shininess", u->materialParams.x);
    pipe.program->setFloat("uMaterail.shininess_strength", u->materialParams.y);
    pipe.program->setBool("uMaterail.has_normal_tex", u->materialParams.z > 0.5f);
    pipe.program->setBool("uMaterail.has_specular_tex", u->materialParams.w > 0.5f);
    pipe.program->setFloat("uOpacity", u->misc.x);   // D3：不透明度
    // V3-19：阴影——光空间矩阵 + 采样开关（VK 从 UBO 同布局字段读取，GL 按名拆解）
    pipe.program->setMat4("uLightSpaceMatrix", &u->lightVP[0][0]);
    pipe.program->setInt("uShadowEnabled", u->misc.y > 0.5f ? 1 : 0);
}

void GLCommandList::draw(uint32_t vertexCount, uint32_t firstVertex) {
    if (!_curPipeline.valid() || !_curVertexBuffer.valid() || _formatMismatch) return;
    const GLuint vao = obtainVAO();
    if (!vao) return;
    const GLPipeline& pipe = _device->pipeline(_curPipeline);
    pipe.program->use();
    flushUniforms();
    glBindVertexArray(vao);
    glDrawArrays(mapPrimitive(pipe.desc.primitive), firstVertex, vertexCount);
}

void GLCommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex) {
    if (!_curPipeline.valid() || !_curVertexBuffer.valid() || !_curIndexBuffer.valid()) return;
    if (_formatMismatch) return;
    const GLuint vao = obtainVAO();
    if (!vao) return;
    const GLPipeline& pipe = _device->pipeline(_curPipeline);
    pipe.program->use();
    flushUniforms();
    glBindVertexArray(vao);
    // ELEMENT_ARRAY_BUFFER 是 VAO 状态，缓存命中时也要确保指向当前索引流
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _device->buffer(_curIndexBuffer).id);
    const GLenum primitive = mapPrimitive(pipe.desc.primitive);
    const GLenum indexType = GL_UNSIGNED_INT;   // v1 固定 uint32 索引（与现有 MeshData 一致）
    if (instanceCount > 1) {
        glDrawElementsInstanced(primitive, indexCount, indexType,
                                reinterpret_cast<const void*>(static_cast<uintptr_t>(firstIndex) * sizeof(uint32_t)),
                                instanceCount);
    } else {
        glDrawElements(primitive, indexCount, indexType,
                       reinterpret_cast<const void*>(static_cast<uintptr_t>(firstIndex) * sizeof(uint32_t)));
    }
}

void GLCommandList::pushDebugLabel(const char* label) {
    // 需要 GL_KHR_debug 扩展支持；这里保守处理
#ifdef GL_KHR_debug
    glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0, -1, label);
#else
    (void)label;
#endif
}

void GLCommandList::popDebugLabel() {
#ifdef GL_KHR_debug
    glPopDebugGroup();
#endif
}

} // namespace rhi
