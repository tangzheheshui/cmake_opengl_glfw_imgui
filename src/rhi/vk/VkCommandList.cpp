//
//  VkCommandList.cpp
//
#include "log/LogMacros.h"
#include "VkCommandList.h"
#include "VkDevice.h"

namespace rhi {

VkCommandList::VkCommandList(VulkanDevice* device, bool primary)
    : _device(device)
    , _primary(primary)
    , _uniformOffset(device->defaultUniformOffset()) {
}

VkCommandList::~VkCommandList() {
    if (_cmdBuffers.empty()) return;
    VkCommandPool pool = _device->commandPool();
    if (pool != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(_device->vkDeviceHandle(), pool,
                             static_cast<uint32_t>(_cmdBuffers.size()), _cmdBuffers.data());
    }
    _cmdBuffers.clear();
}

bool VkCommandList::initCommandBuffers(uint32_t slots) {
    if (slots < 1) slots = 1;
    _cmdBuffers.resize(slots, VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool        = _device->commandPool();
    ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = slots;
    if (vkAllocateCommandBuffers(_device->vkDeviceHandle(), &ai, _cmdBuffers.data()) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] command list buffers alloc failed");
        _cmdBuffers.clear();
        return false;
    }
    return true;
}

void VkCommandList::beginFrameSlot(uint32_t slot) {
    if (slot >= _cmdBuffers.size()) return;
    _slot = slot;
    _used = false;
    if (vkResetCommandBuffer(_cmdBuffers[slot], 0) != VK_SUCCESS) return;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    if (vkBeginCommandBuffer(_cmdBuffers[slot], &bi) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] command list begin failed (slot %u)", slot);
        return;
    }
    // 绑定状态按帧归零：新命令缓冲里旧绑定不再有效
    for (uint32_t i = 0; i < kMaxTexSlots; ++i) _texSlots[i] = TextureHandle{};
    _uniformOffset = _device->defaultUniformOffset();
    for (auto& s : _lastSets) s = VK_NULL_HANDLE;
    _lastOffset  = UINT32_MAX;
    _pushDirty   = false;
    _curPipeline = PipelineHandle{};
}

void VkCommandList::beginRenderPass(const RenderPassDesc& desc) {
    _used = true;
    // B4：离屏 RenderTarget 分支（dynamic rendering 附件来自 RT 资源）
    if (desc.renderTarget.valid()) {
        const VkRenderTargetRes& rt = _device->renderTargetRes(desc.renderTarget);
        if (!rt.alive || !rt.colorTex.valid()) {
            ENGINE_LOG_WARN_ONCE("[VK] beginRenderPass: dead/invalid render target");
            return;
        }
        const VkExtent2D extent{rt.width, rt.height};

        VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        colorAtt.imageView   = _device->textureRes(rt.colorTex).view;
        colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorAtt.loadOp      = desc.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        colorAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.clearValue.color = {{ desc.clearColorValue[0], desc.clearColorValue[1],
                                       desc.clearColorValue[2], desc.clearColorValue[3] }};

        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.offset = {0, 0};
        ri.renderArea.extent = extent;
        ri.layerCount        = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments    = &colorAtt;

        VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        if (rt.depthView) {
            depthAtt.imageView   = rt.depthView;
            // 1.0 布局（DEPTH_ATTACHMENT_OPTIMAL 需 1.2 扩展，见 recordRenderTargetBeginBarriers）
            depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAtt.loadOp      = desc.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            // B3b：VK 后端深度语义反转，清屏值固定 0（最远），与 swapchain 路径一致
            depthAtt.clearValue.depthStencil = {0.0f, 0};
            ri.pDepthAttachment = &depthAtt;
        }

        _device->recordRenderTargetBeginBarriers(commandBuffer(), desc.renderTarget);
        _device->beginRenderingFn()(commandBuffer(), &ri);
        _inRenderPass    = true;
        _curRenderTarget = desc.renderTarget;

        // 与 swapchain 路径相同的负高度 viewport（B3a）+ scissor
        setViewport(0, static_cast<float>(extent.height),
                    static_cast<float>(extent.width), -static_cast<float>(extent.height));
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(commandBuffer(), 0, 1, &scissor);
        return;
    }

    // 没有离屏目标 = 默认帧缓冲（swapchain）。离屏目标由上面的分支处理。
    _curRenderTarget = RenderTargetHandle{};

    // B1：Dynamic Rendering —— vkCmdBeginRendering 取代 VkRenderPass/framebuffer。
    // loadOp 按 desc 决定（CLEAR/LOAD），store 恒 STORE（颜色）/DONT_CARE（深度）。
    // 附件布局转换由 device 布局跟踪屏障完成（recordBeginPassBarriers）。
    const VkExtent2D extent = _device->swapchainExtent();

    VkRenderingAttachmentInfo colorAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    colorAtt.imageView   = _device->currentColorView();
    colorAtt.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAtt.loadOp      = desc.clearColor ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAtt.storeOp     = VK_ATTACHMENT_STORE_OP_STORE;
    colorAtt.clearValue.color = {{ desc.clearColorValue[0], desc.clearColorValue[1],
                                   desc.clearColorValue[2], desc.clearColorValue[3] }};

    VkRenderingAttachmentInfo depthAtt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    depthAtt.imageView   = _device->depthImageView();
    depthAtt.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAtt.loadOp      = desc.clearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAtt.storeOp     = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // B3b Reversed-Z：VK 后端深度语义反转（1=近平面 / 0=远平面），清屏值固定 0（最远）。
    // GL 路径的 desc.depthClearValue=1 约定在此后端内转换，调用方无需感知。
    depthAtt.clearValue.depthStencil = {0.0f, 0};

    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.offset = {0, 0};
    ri.renderArea.extent = extent;
    ri.layerCount        = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments    = &colorAtt;
    ri.pDepthAttachment     = &depthAtt;

    _device->recordBeginPassBarriers(commandBuffer(), desc.clearColor, desc.clearDepth);
    _device->beginRenderingFn()(commandBuffer(), &ri);
    _inRenderPass = true;

    // 动态 viewport/scissor（desc 未指定时用 swapchain 尺寸）
    uint32_t w = desc.viewportWidth  ? desc.viewportWidth  : extent.width;
    uint32_t h = desc.viewportHeight ? desc.viewportHeight : extent.height;
    // B3a：负高度 viewport——网格按 GL 约定（NDC y 向上）提交，Vulkan NDC y 向下，
    // 负高度视口把投屏上下翻转，3D 内容不再相对 UI 倒置（origin.y=高、height=-高）。
    // 负高度需要 maintenance1（Vulkan 1.1 起为核心）；镜像同时翻转屏幕绕序，
    // 管线侧配套 frontFace=CLOCKWISE（见 VulkanDevice::createPipeline）。
    // ImGui 自设正高度 viewport + 自带投影（动态状态逐命令生效），互不影响。
    setViewport(0, static_cast<float>(h), static_cast<float>(w), -static_cast<float>(h));
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetScissor(commandBuffer(), 0, 1, &scissor);
}

void VkCommandList::endRenderPass() {
    if (!_inRenderPass) return;
    // B4：离屏 RT 收尾 → 颜色附件转回 SHADER_READ_ONLY（采样/回读就绪）
    // V3-0：屏障必须记录在 vkCmdEndRendering 之后——rendering 实例内禁止
    // 图像布局转换（验证层 6 条 error 的来源），实例外记录即合法
    const bool hadRT = _curRenderTarget.valid();
    _device->endRenderingFn()(commandBuffer());
    if (hadRT) {
        _device->recordRenderTargetEndBarriers(commandBuffer(), _curRenderTarget);
        _curRenderTarget = RenderTargetHandle{};
    }
    _inRenderPass = false;
}

void VkCommandList::bindPipeline(PipelineHandle pipeline) {
    if (!pipeline.valid()) return;
    _used = true;
    _curPipeline = pipeline;
    // 关键：真正绑定管线命令（漏掉此调用时 encoder 无绑定管线，
    // draw 编码时 getGraphicsPipeline() 返回 null 直接崩溃）
    vkCmdBindPipeline(commandBuffer(),
                      VK_PIPELINE_BIND_POINT_GRAPHICS,
                      _device->pipelineRes(pipeline).pipeline);
    const VkPipelineRes& p = _device->pipelineRes(pipeline);
    std::memcpy(_pushData, p.pushConstants, sizeof(_pushData));   // 以管线初始值打底
    _pushDirty = true;
}

// ---- B3b：GL 约定 clip 空间 → Vulkan Reversed-Z [0,1] ----
// GL 投影把视锥映射到 z∈[-1,1]（近=-1/远=+1），Vulkan 裁剪体是 z∈[0,1]。
// 对"含投影矩阵"的 mat4 做后乘修正：z_clip' = 0.5*(w_clip - z_clip)，即
// 近平面 → 1（最近）、远平面 → 0（最远），配合管线 GREATER 比较 + 清屏 0
// （Reversed-Z：浮点深度精度集中在近端，CAD 大深跨场景必需）。
// 仅修正 MVP 类矩阵；uViewMat 是纯视图矩阵（天空盒），不参与深度映射，保持原样。
static void applyReversedZCorrection(float* m16) {
    for (int c = 0; c < 4; ++c) {
        float*       z = m16 + c * 4 + 2;   // glm 列主序：元素[row][col]在 col*4+row
        const float* w = m16 + c * 4 + 3;
        *z = 0.5f * (*w - *z);   // z 行：z' = 0.5*w - 0.5*z；w 行不变
    }
}

void VkCommandList::setUniform(const char* name, const UniformValue& value) {
    // B6：优先查当前管线的反射表（创建期从 SPIR-V 反射出 名字→偏移/类型）。
    // 命中即写，不再依赖字符串→偏移硬编码（漏配即静默丢失的坑由此消灭）。
    if (_curPipeline.valid()) {
        const auto& table = _device->pipelineRes(_curPipeline).uniformTable;
        for (const auto& u : table) {
            if (std::strcmp(u.name, name) != 0) continue;
            float* dst = _pushData + u.offset / sizeof(float);
            if (u.isMat4) {
                std::memcpy(dst, value.data, 16 * sizeof(float));
                // Reversed-Z 修正只作用于投影类矩阵（uProjMat 天空盒/uMatrixMVP/
                // uViewProj）；纯视图矩阵（uViewMat）不做。判别 = 名字含 "Proj"
                // 或 "MVP"——与旧回退映射一致（B6 反射表曾漏掉后者，宽线是
                // 唯一用 uMatrixMVP 且开深度测试的管线，z 域错乱被裁剪/遮挡）
                if (std::strstr(name, "Proj") != nullptr
                    || std::strstr(name, "MVP") != nullptr) {
                    applyReversedZCorrection(dst);
                }
            } else if (value.type == UniformValue::Type::Vec3) {
                // 与旧 uColor 语义一致：Vec3 写入补 alpha=1
                std::memcpy(dst, value.data, 3 * sizeof(float));
                dst[3] = 1.0f;
            } else {
                const uint32_t n = std::min<uint32_t>(u.size / sizeof(float), 4u);
                std::memcpy(dst, value.data, n * sizeof(float));
            }
            _pushDirty = true;
            return;
        }
    }

    // ---- 回退：反射表为空时走旧约定映射（保底，勿删） ----
    if (std::strcmp(name, "uViewMat") == 0) {
        // 天空盒视图矩阵（offset 0）：不做 Reversed-Z 修正，见 applyReversedZCorrection 注释
        std::memcpy(_pushData + kPushMvpOffset / sizeof(float), value.data, 16 * sizeof(float));
        _pushDirty = true;
    } else if (std::strcmp(name, "uMatrixMVP") == 0 || std::strcmp(name, "uViewProj") == 0) {
        std::memcpy(_pushData + kPushMvpOffset / sizeof(float), value.data, 16 * sizeof(float));
        applyReversedZCorrection(_pushData + kPushMvpOffset / sizeof(float));
        _pushDirty = true;
    } else if (std::strcmp(name, "uProjMat") == 0) {
        // 天空盒：第二个 mat4 在 push 区 offset 64（含投影矩阵 → 同样修正深度映射）
        std::memcpy(_pushData + 64 / sizeof(float), value.data, 16 * sizeof(float));
        applyReversedZCorrection(_pushData + 64 / sizeof(float));
        _pushDirty = true;
    } else if (std::strcmp(name, "uClipPlane") == 0 || std::strcmp(name, "uPickColor") == 0) {
        // D7 剖切平面 / D2 拾取色：vec4 放在 offset 64（与天空盒 uProjMat 同区，但管线不同）
        std::memcpy(_pushData + 64 / sizeof(float), value.data, 4 * sizeof(float));
        _pushDirty = true;
    } else if (std::strcmp(name, "uColor") == 0) {
        float rgba[4] = { value.data[0], value.data[1], value.data[2], 1.0f };
        std::memcpy(_pushData + kPushColorOffset / sizeof(float), rgba, 4 * sizeof(float));
        _pushDirty = true;
    } else if (std::strcmp(name, "uTex") == 0) {
        // GL 采样器 uniform 编号；VK 走 bindTexture 槽位，忽略
    } else {
        // B6：反射表已命中的名字不会走到这里；未命中且表为空（无 PC 着色器）
        // 或反射缺名时提示一次（不再吞掉所有未知名）
        static const char* lastName = nullptr;
        if (lastName != name) {
            ENGINE_LOG_WARN_ONCE("[VK] unknown uniform '%s' (ignored)", name);
            lastName = name;
        }
    }
}

void VkCommandList::bindVertexBuffer(BufferHandle buffer, uint32_t binding) {
    if (!buffer.valid()) return;
    if (binding == 0) _curVertexBuffer = buffer;
    VkBuffer vb = _device->bufferRes(buffer).buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(commandBuffer(), binding, 1, &vb, &offset);
}

void VkCommandList::bindIndexBuffer(BufferHandle buffer) {
    if (!buffer.valid()) return;
    _curIndexBuffer = buffer;
    vkCmdBindIndexBuffer(commandBuffer(), _device->bufferRes(buffer).buffer,
                         0, VK_INDEX_TYPE_UINT32);
}

void VkCommandList::bindTexture(TextureHandle texture, uint32_t slot) {
    // V3-2：只记录槽位。纹理在创建期已写好自己的描述符集（sampler + view 不变），
    // draw 时直接绑 set1..4 —— 这里不再 vkUpdateDescriptorSets。
    if (!texture.valid() || slot >= kMaxTexSlots) return;
    _texSlots[slot] = texture;
}

void VkCommandList::setDrawUniforms(UniformBlockHandle /*block*/, const void* data, uint32_t size) {
    _used = true;
    // V3-2：数据追加进帧内 uniform 环（256B 对齐），draw 时作为 set0 的动态偏移。
    // block 句柄不再承载 GPU 数据（GL 后端本来就是哑句柄）。
    if (!data || size == 0) return;
    const uint32_t off = _device->allocDrawUniforms(data, size);
    if (off != UINT32_MAX) _uniformOffset = off;
    else ENGINE_LOG_WARN_ONCE("[VK] draw uniform ring exhausted; draw uses stale block");
}

void VkCommandList::setViewport(float x, float y, float w, float h) {
    // B3a：本后端统一用负高度 viewport 实现 GL 约定的 y 向上
    // （beginRenderPass 传入 y=+height / h=-height；maintenance1，Vulkan 1.1 核心）。
    // ImGui 侧用自己的正高度 viewport 录制（动态状态，逐命令生效，互不影响）。
    VkViewport vp{x, y, w, h, 0.0f, 1.0f};
    vkCmdSetViewport(commandBuffer(), 0, 1, &vp);
}

// C2：裁剪矩形。VK 帧缓冲坐标即左上原点，与契约一致，直接透传。
// （负高度 viewport 只翻转光栅化 y，scissor 仍在帧缓冲坐标系）
void VkCommandList::setScissor(int x, int y, int w, int h) {
    VkRect2D sc{{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    vkCmdSetScissor(commandBuffer(), 0, 1, &sc);
}

void VkCommandList::applyPushConstants() {
    if (_pushDirty && _curPipeline.valid()) {
        vkCmdPushConstants(commandBuffer(), _device->pipelineLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, kPushTotalSize, _pushData);
        _pushDirty = false;
    }
}

// V3-2：一次绑 5 个描述符集（set0..4），动态偏移只有 set0 一个。
// 纹理槽没绑时用白纹理兜底（避免描述符未定义）。
// 相邻绘制常共用同一组纹理 + 同一块 uniform，缓存命中就跳过绑定命令。
void VkCommandList::bindDescriptorState() {
    VkDescriptorSet sets[1 + kMaxTexSlots];
    sets[0] = _device->frameUniformSet();
    for (uint32_t i = 0; i < kMaxTexSlots; ++i) {
        const VkDescriptorSet texSet =
            _device->textureSet(_texSlots[i].valid() ? _texSlots[i] : TextureHandle{});
        sets[i + 1] = texSet ? texSet : _device->fallbackTextureSet(i);
    }

    bool same = (_lastOffset == _uniformOffset);
    for (uint32_t i = 0; same && i < 1 + kMaxTexSlots; ++i) {
        same = (_lastSets[i] == sets[i]);
    }
    if (same) return;

    const uint32_t dynOffset = _uniformOffset;
    vkCmdBindDescriptorSets(commandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            _device->pipelineLayout(), 0, 1 + kMaxTexSlots,
                            sets, 1, &dynOffset);
    for (uint32_t i = 0; i < 1 + kMaxTexSlots; ++i) _lastSets[i] = sets[i];
    _lastOffset = _uniformOffset;
}

void VkCommandList::draw(uint32_t vertexCount, uint32_t firstVertex) {
    if (!_inRenderPass || !_curPipeline.valid()) return;
    _used = true;
    applyPushConstants();
    bindDescriptorState();
    vkCmdDraw(commandBuffer(), vertexCount, 1, firstVertex, 0);
}

void VkCommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex) {
    if (!_inRenderPass || !_curPipeline.valid() || !_curIndexBuffer.valid()) return;
    _used = true;
    applyPushConstants();
    bindDescriptorState();
    vkCmdDrawIndexed(commandBuffer(), indexCount, instanceCount, firstIndex, 0, 0);
}

void VkCommandList::pushDebugLabel(const char*) {}
void VkCommandList::popDebugLabel() {}

uint64_t VkCommandList::nativeCommandBuffer() const {
    return reinterpret_cast<uint64_t>(commandBuffer());
}

} // namespace rhi
