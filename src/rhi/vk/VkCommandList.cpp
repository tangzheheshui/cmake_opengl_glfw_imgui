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
    clears[1].depthStencil = {desc.depthClearValue, 0};
    bi.clearValueCount = 2;   // 颜色 + 深度（M4 深度附件）
    bi.pClearValues    = clears;

    vkCmdBeginRenderPass(_device->commandBuffer(), &bi, VK_SUBPASS_CONTENTS_INLINE);
    _inRenderPass = true;

    // 动态 viewport/scissor（desc 未指定时用 swapchain 尺寸）
    uint32_t w = desc.viewportWidth  ? desc.viewportWidth  : bi.renderArea.extent.width;
    uint32_t h = desc.viewportHeight ? desc.viewportHeight : bi.renderArea.extent.height;
    setViewport(0, 0, static_cast<float>(w), static_cast<float>(h));
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

void VkCommandList::setUniform(const char* name, const UniformValue& value) {
    // M3 固定映射（与 res/shaders_vk 的 push_constant 块一致）：
    //   uMatrixMVP → offset 0（mat4）
    //   uColor     → offset 64（vec4；Vec3 补 alpha=1）
    if (std::strcmp(name, "uMatrixMVP") == 0 || std::strcmp(name, "uViewProj") == 0
        || std::strcmp(name, "uViewMat") == 0) {
        std::memcpy(_pushData + kPushMvpOffset / sizeof(float), value.data, 16 * sizeof(float));
        _pushDirty = true;
    } else if (std::strcmp(name, "uProjMat") == 0) {
        // 天空盒：第二个 mat4 在 push 区 offset 64
        std::memcpy(_pushData + 64 / sizeof(float), value.data, 16 * sizeof(float));
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
    // 注意：Vulkan 的 NDC y 轴向下，与 GL 相反。
    // M3 暂用正高度 viewport（内容上下翻转对冒烟三角形验证无影响——
    // 颜色对称，中心像素不变）；正式翻转方案（负高度或 shader 里翻 y）M4 决定
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
