//
//  VkCommandList.cpp
//
#include "log/LogMacros.h"
#include "VkCommandList.h"
#include "VkDevice.h"

namespace rhi {

VkCommandList::VkCommandList(VulkanDevice* device)
    : _device(device)
    , _curUniformBlock(device->defaultUniformBlock()) {
}

void VkCommandList::beginRenderPass(const RenderPassDesc& desc) {
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

        _device->recordRenderTargetBeginBarriers(_device->commandBuffer(), desc.renderTarget);
        _device->beginRenderingFn()(_device->commandBuffer(), &ri);
        _inRenderPass    = true;
        _curRenderTarget = desc.renderTarget;

        // 与 swapchain 路径相同的负高度 viewport（B3a）+ scissor
        setViewport(0, static_cast<float>(extent.height),
                    static_cast<float>(extent.width), -static_cast<float>(extent.height));
        VkRect2D scissor{{0, 0}, extent};
        vkCmdSetScissor(_device->commandBuffer(), 0, 1, &scissor);
        return;
    }

    // M3：仅支持默认帧缓冲（swapchain）；desc.framebufferId 的 FBO 直通是 GL 专属能力，
    // Vulkan 侧等价物（RenderTargetHandle）已由上面的 B4 分支处理
    if (desc.framebufferId != 0) {
        static bool warned = false;
        if (!warned) {
            ENGINE_LOG_WARN_ONCE("[VK] non-default framebuffer not supported yet (B4)");
            warned = true;
        }
        return;
    }
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

    _device->recordBeginPassBarriers(_device->commandBuffer(), desc.clearColor, desc.clearDepth);
    _device->beginRenderingFn()(_device->commandBuffer(), &ri);
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
    vkCmdSetScissor(_device->commandBuffer(), 0, 1, &scissor);
}

void VkCommandList::endRenderPass() {
    if (!_inRenderPass) return;
    // B4：离屏 RT 收尾 → 颜色附件转回 SHADER_READ_ONLY（采样/回读就绪）
    // V3-0：屏障必须记录在 vkCmdEndRendering 之后——rendering 实例内禁止
    // 图像布局转换（验证层 6 条 error 的来源），实例外记录即合法
    const bool hadRT = _curRenderTarget.valid();
    _device->endRenderingFn()(_device->commandBuffer());
    if (hadRT) {
        _device->recordRenderTargetEndBarriers(_device->commandBuffer(), _curRenderTarget);
        _curRenderTarget = RenderTargetHandle{};
    }
    _inRenderPass = false;
}

void VkCommandList::bindPipeline(PipelineHandle pipeline) {
    if (!pipeline.valid()) return;
    _curPipeline = pipeline;
    // 关键：真正绑定管线命令（漏掉此调用时 encoder 无绑定管线，
    // draw 编码时 getGraphicsPipeline() 返回 null 直接崩溃）
    vkCmdBindPipeline(_device->commandBuffer(),
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
                // uViewProj）；纯视图矩阵（uViewMat）不做——名字含 "Proj" 判别
                if (std::strstr(name, "Proj") != nullptr) {
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
    vkCmdBindVertexBuffers(_device->commandBuffer(), binding, 1, &vb, &offset);
}

void VkCommandList::bindIndexBuffer(BufferHandle buffer) {
    if (!buffer.valid()) return;
    _curIndexBuffer = buffer;
    vkCmdBindIndexBuffer(_device->commandBuffer(), _device->bufferRes(buffer).buffer,
                         0, VK_INDEX_TYPE_UINT32);
}

void VkCommandList::bindTexture(TextureHandle texture, uint32_t slot) {
    if (!texture.valid() || slot >= 4) return;
    const VkTextureRes& t = _device->textureRes(texture);
    VkDescriptorImageInfo info{};
    info.sampler     = t.sampler;
    info.imageView   = t.view;
    info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = _device->uniformBlockRes(_curUniformBlock).set;
    w.dstBinding      = slot + 1;   // binding0 是绘制级 UBO，采样器从 1 起
    w.dstArrayElement = 0;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo      = &info;
    vkUpdateDescriptorSets(_device->vkDeviceHandle(), 1, &w, 0, nullptr);
}

void VkCommandList::setDrawUniforms(UniformBlockHandle block, const void* data, uint32_t size) {
    if (!data || size > kDrawUniformsSize) return;
    if (!block.valid()) block = _curUniformBlock;   // 未指定则沿用当前块
    std::memcpy(_device->uniformBlockRes(block).mapped, data, size);
    _curUniformBlock = block;
}

void VkCommandList::setViewport(float x, float y, float w, float h) {
    // B3a：本后端统一用负高度 viewport 实现 GL 约定的 y 向上
    // （beginRenderPass 传入 y=+height / h=-height；maintenance1，Vulkan 1.1 核心）。
    // ImGui 侧用自己的正高度 viewport 录制（动态状态，逐命令生效，互不影响）。
    VkViewport vp{x, y, w, h, 0.0f, 1.0f};
    vkCmdSetViewport(_device->commandBuffer(), 0, 1, &vp);
}

// C2：裁剪矩形。VK 帧缓冲坐标即左上原点，与契约一致，直接透传。
// （负高度 viewport 只翻转光栅化 y，scissor 仍在帧缓冲坐标系）
void VkCommandList::setScissor(int x, int y, int w, int h) {
    VkRect2D sc{{x, y}, {static_cast<uint32_t>(w), static_cast<uint32_t>(h)}};
    vkCmdSetScissor(_device->commandBuffer(), 0, 1, &sc);
}

void VkCommandList::applyPushConstants() {
    if (_pushDirty && _curPipeline.valid()) {
        vkCmdPushConstants(_device->commandBuffer(), _device->pipelineLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, kPushTotalSize, _pushData);
        _pushDirty = false;
    }
    // uniform block 描述符集（binding0=UBO 数据，1..4=纹理），每 draw 绑定
    // B4：未显式 setDrawUniforms 时回退设备默认块（防御无效句柄越界）
    UniformBlockHandle blk = _curUniformBlock.valid() ? _curUniformBlock
                                                      : _device->defaultUniformBlock();
    VkDescriptorSet set = _device->uniformBlockRes(blk).set;
    vkCmdBindDescriptorSets(_device->commandBuffer(), VK_PIPELINE_BIND_POINT_GRAPHICS,
                            _device->pipelineLayout(), 0, 1, &set, 0, nullptr);
}

void VkCommandList::draw(uint32_t vertexCount, uint32_t firstVertex) {
    if (!_inRenderPass || !_curPipeline.valid()) return;
    applyPushConstants();
    vkCmdDraw(_device->commandBuffer(), vertexCount, 1, firstVertex, 0);
}

void VkCommandList::drawIndexed(uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex) {
    if (!_inRenderPass || !_curPipeline.valid() || !_curIndexBuffer.valid()) return;
    applyPushConstants();
    vkCmdDrawIndexed(_device->commandBuffer(), indexCount, instanceCount, firstIndex, 0, 0);
}

void VkCommandList::pushDebugLabel(const char*) {}
void VkCommandList::popDebugLabel() {}

uint64_t VkCommandList::nativeCommandBuffer() const {
    return reinterpret_cast<uint64_t>(_device->commandBuffer());
}

} // namespace rhi
