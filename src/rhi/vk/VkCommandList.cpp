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
    // M3：仅支持默认帧缓冲（swapchain）；desc.framebufferId 的 FBO 直通是 GL 专属能力，
    // Vulkan 侧等价物（TextureHandle framebuffer）在 M4 引入
    if (desc.framebufferId != 0) {
        static bool warned = false;
        if (!warned) {
            ENGINE_LOG_WARN_ONCE("[VK] non-default framebuffer not supported yet (M4)");
            warned = true;
        }
        return;
    }

    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass        = _device->renderPass();
    bi.framebuffer       = _device->currentFramebuffer();
    bi.renderArea.offset = {0, 0};
    bi.renderArea.extent = _device->swapchainExtent();

    VkClearValue clears[2];
    clears[0].color = {{ desc.clearColorValue[0], desc.clearColorValue[1],
                         desc.clearColorValue[2], desc.clearColorValue[3] }};
    // B3b Reversed-Z：VK 后端深度语义反转（1=近平面 / 0=远平面），清屏值固定 0（最远）。
    // GL 路径的 desc.depthClearValue=1 约定在此后端内转换，调用方无需感知。
    clears[1].depthStencil = {0.0f, 0};
    bi.clearValueCount = 2;   // 颜色 + 深度（M4 深度附件）
    bi.pClearValues    = clears;

    vkCmdBeginRenderPass(_device->commandBuffer(), &bi, VK_SUBPASS_CONTENTS_INLINE);
    _inRenderPass = true;

    // 动态 viewport/scissor（desc 未指定时用 swapchain 尺寸）
    uint32_t w = desc.viewportWidth  ? desc.viewportWidth  : bi.renderArea.extent.width;
    uint32_t h = desc.viewportHeight ? desc.viewportHeight : bi.renderArea.extent.height;
    // B3a：负高度 viewport——网格按 GL 约定（NDC y 向上）提交，Vulkan NDC y 向下，
    // 负高度视口把投屏上下翻转，3D 内容不再相对 UI 倒置（origin.y=高、height=-高）。
    // 负高度需要 maintenance1（Vulkan 1.1 起为核心）；镜像同时翻转屏幕绕序，
    // 管线侧配套 frontFace=CLOCKWISE（见 VulkanDevice::createPipeline）。
    setViewport(0, static_cast<float>(h), static_cast<float>(w), -static_cast<float>(h));
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetScissor(_device->commandBuffer(), 0, 1, &scissor);
}

void VkCommandList::endRenderPass() {
    if (!_inRenderPass) return;
    vkCmdEndRenderPass(_device->commandBuffer());
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
    // M3 固定映射（与 res/shaders_vk 的 push_constant 块一致）：
    //   uMatrixMVP → offset 0（mat4）
    //   uColor     → offset 64（vec4；Vec3 补 alpha=1）
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
    } else if (std::strcmp(name, "uClipPlane") == 0) {
        // D7 剖切平面：vec4 放在 offset 64（与天空盒 uProjMat 同区，但管线不同）
        std::memcpy(_pushData + 64 / sizeof(float), value.data, 4 * sizeof(float));
        _pushDirty = true;
    } else if (std::strcmp(name, "uColor") == 0) {
        float rgba[4] = { value.data[0], value.data[1], value.data[2], 1.0f };
        std::memcpy(_pushData + kPushColorOffset / sizeof(float), rgba, 4 * sizeof(float));
        _pushDirty = true;
    } else if (std::strcmp(name, "uTex") == 0) {
        // GL 采样器 uniform 编号；VK 走 bindTexture 槽位，忽略
    } else {
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

void VkCommandList::applyPushConstants() {
    if (_pushDirty && _curPipeline.valid()) {
        vkCmdPushConstants(_device->commandBuffer(), _device->pipelineLayout(),
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, kPushTotalSize, _pushData);
        _pushDirty = false;
    }
    // uniform block 描述符集（binding0=UBO 数据，1..4=纹理），每 draw 绑定
    VkDescriptorSet set = _device->uniformBlockRes(_curUniformBlock).set;
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
