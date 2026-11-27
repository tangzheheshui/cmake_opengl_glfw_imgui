//
//  VkDeviceResources.cpp —— 缓冲/离屏目标/回读/uniform 块
//  （从 VkDevice.cpp 按职责拆出，逻辑不变）
//
#include "VkDevice.h"
#include "VkDeviceInternal.h"
#include "../FormatInfo.h"
#include "log/LogMacros.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rhi {

BufferHandle VulkanDevice::createBuffer(const BufferDesc& desc) {
    if (_deviceLost) return {};
    VkBufferRes res;
    res.size = desc.size;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = desc.size;
    bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT
              | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK_RET(vkCreateBuffer(_device, &bci, nullptr, &res.buffer));

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(_device, res.buffer, &req);
    // E1：HOST_VISIBLE 子分配（页持久映射，mapped 直接可得，省 per-object map）
    res.memAlloc = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                       | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!res.memAlloc.valid()) {
        ENGINE_LOG_ERROR("[VK] buffer memory alloc failed (%u bytes)", desc.size);
        vkDestroyBuffer(_device, res.buffer, nullptr);
        return {};
    }
    res.memory = res.memAlloc.memory;
    res.mapped = res.memAlloc.mapped;
    if (vkBindBufferMemory(_device, res.buffer, res.memory, res.memAlloc.offset) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] vkBindBufferMemory failed at %s:%d", __FILE__, __LINE__);
        vkDestroyBuffer(_device, res.buffer, nullptr);
        _memAlloc.free(res.memAlloc);
        return {};
    }
    if (desc.initialData && desc.size > 0) {
        std::memcpy(res.mapped, desc.initialData, desc.size);
    }

    RhiHandle base = _buffers.alloc(std::move(res));
    BufferHandle h; h.id = base.id; h.gen = base.gen;
    char name[48];
    std::snprintf(name, sizeof(name), "RHI_Buffer_%u", h.id);
    setObjName(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(res.buffer), name);
    return h;
}

void VulkanDevice::updateBuffer(BufferHandle buffer, const void* data,
                            uint32_t size, uint32_t offset) {
    if (!_buffers.alive(buffer) || !data) return;
    VkBufferRes& b = _buffers.get(buffer.id);
    if (!b.mapped || offset + size > b.size) return;   // 已销毁 / 越界保护
    std::memcpy(static_cast<uint8_t*>(b.mapped) + offset, data, size);
}

void VulkanDevice::destroyBuffer(BufferHandle h) {
    if (!_buffers.alive(h)) return;
    VkBufferRes& b = _buffers.get(h.id);
    if (!b.buffer) return;
    VkBuffer buf = b.buffer;
    VkMemAlloc mem = b.memAlloc;
    _buffers.free(h.id);
    _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                   [this, buf, mem] {
                       vkDestroyBuffer(_device, buf, nullptr);
                       _memAlloc.free(mem);
                   });
}

void VulkanDevice::destroyTexture(TextureHandle h) {
    if (!_textures.alive(h)) return;
    VkTextureRes& t = _textures.get(h.id);
    if (!t.image) return;
    VkImage img = t.image;
    VkImageView view = t.view;
    VkSampler sm = t.sampler;
    VkMemAlloc mem = t.memAlloc;
    _textures.free(h.id);
    _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                   [this, img, view, sm, mem] {
                       if (sm)   vkDestroySampler(_device, sm, nullptr);
                       if (view) vkDestroyImageView(_device, view, nullptr);
                       vkDestroyImage(_device, img, nullptr);
                       _memAlloc.free(mem);
                   });
}

void VulkanDevice::destroyPipeline(PipelineHandle h) {
    if (!_pipelines.alive(h)) return;
    VkPipelineRes& p = _pipelines.get(h.id);
    if (p.pipeline) {
        VkPipeline pipe = p.pipeline;
        // V3-0：管线可能被帧在飞的命令引用，同样延迟销毁
        _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                       [this, pipe] { vkDestroyPipeline(_device, pipe, nullptr); });
    }
    _pipelines.free(h.id);   // 标记 dead + gen++（swapchain 重建不复活）
}

void VulkanDevice::destroyUniformBlock(UniformBlockHandle h) {
    if (!_uniformBlocks.alive(h)) return;
    if (h.id == _defaultUniformBlock.id) {
        ENGINE_LOG_WARN_ONCE("[VK] destroyUniformBlock: device default block is device-owned, ignored");
        return;
    }
    // V3-2：block 没有 GPU 资源，释放槽位即可
    _uniformBlocks.free(h.id);
}

RenderTargetHandle VulkanDevice::createRenderTarget(const RenderTargetDesc& desc) {
    if (_deviceLost) return {};
    if (desc.width == 0 || desc.height == 0) {
        ENGINE_LOG_ERROR("[VK] createRenderTarget: zero-sized target");
        return {};
    }
    VkRenderTargetRes rt;
    rt.width = desc.width;
    rt.height = desc.height;
    rt.hasColor = desc.hasColor;
    rt.samples = desc.samples > 0 ? desc.samples : 1;
    rt.colorFormat = desc.colorFormat;
    rt.depthFormat = desc.depthFormat;

    // 颜色附件走纹理机制：采样（bindTexture/描述符）、附件、回读（TRANSFER_SRC）一体
    if (desc.hasColor) {
        TextureDesc td;
        td.width  = desc.width;
        td.height = desc.height;
        td.format = desc.colorFormat;
        td.usage  = TEX_SAMPLED | TEX_COLOR_ATTACHMENT;
        td.mipLevels = desc.mipLevels;
        td.sampler.wrapU = td.sampler.wrapV = td.sampler.wrapW = WrapMode::ClampToEdge;
        rt.colorTex = createTexture(td);
        if (!rt.colorTex.valid()) {
            return {};
        }
    }

    // V3-22：MSAA 颜色附件（不能直接采样），另建单采样 resolve 目标作为 colorTex
    if (desc.hasColor && rt.samples > 1) {
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = static_cast<VkFormat>(FormatInfo::of(desc.colorFormat).vkFormat());
        ici.extent        = {desc.width, desc.height, 1};
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        ici.samples       = static_cast<VkSampleCountFlagBits>(rt.samples);
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(_device, &ici, nullptr, &rt.msaaColorImage) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] MSAA RT color image create failed");
            if (rt.colorTex.valid()) destroyTexture(rt.colorTex);
            return {};
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(_device, rt.msaaColorImage, &req);
        rt.msaaColorMem = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (!rt.msaaColorMem.valid() ||
            vkBindImageMemory(_device, rt.msaaColorImage, rt.msaaColorMem.memory,
                              rt.msaaColorMem.offset) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] MSAA RT color memory failed");
            vkDestroyImage(_device, rt.msaaColorImage, nullptr);
            rt.msaaColorImage = VK_NULL_HANDLE;
            if (rt.colorTex.valid()) destroyTexture(rt.colorTex);
            return {};
        }
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image    = rt.msaaColorImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = ici.format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(_device, &vci, nullptr, &rt.msaaColorView) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] MSAA RT color view create failed");
            vkDestroyImage(_device, rt.msaaColorImage, nullptr);
            _memAlloc.free(rt.msaaColorMem);
            rt.msaaColorImage = VK_NULL_HANDLE;
            if (rt.colorTex.valid()) destroyTexture(rt.colorTex);
            return {};
        }
        rt.msaaColorLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // 深度附件：纯深度目标（阴影贴图）走可采样纹理；普通 RT 保留独立 image
    if (desc.hasDepth && !desc.hasColor) {
        TextureDesc dd;
        dd.width  = desc.width;
        dd.height = desc.height;
        dd.format = desc.depthFormat;
        dd.usage  = TEX_DEPTH_ATTACHMENT | TEX_SAMPLED;
        dd.sampler.minFilter = FilterMode::Nearest;   // 深度按最近点采样，比较逻辑在着色器
        dd.sampler.magFilter = FilterMode::Nearest;
        dd.sampler.wrapU = dd.sampler.wrapV = dd.sampler.wrapW = WrapMode::ClampToEdge;
        rt.depthTex = createTexture(dd);
        if (!rt.depthTex.valid()) {
            ENGINE_LOG_ERROR("[VK] depth-only RT: depth texture create failed");
            return {};
        }
    } else if (desc.hasDepth) {
        VkFormat depthFmt = VK_FORMAT_UNDEFINED;
        switch (desc.depthFormat) {
            case Format::Depth32F: depthFmt = VK_FORMAT_D32_SFLOAT; break;
            case Format::D24S8:    depthFmt = VK_FORMAT_D24_UNORM_S8_UINT; break;
            default: depthFmt = findSupportedDepthFormat(_physicalDevice); break;
        }
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.imageType     = VK_IMAGE_TYPE_2D;
        ici.format        = depthFmt;
        ici.extent        = {desc.width, desc.height, 1};
        ici.mipLevels     = 1;
        ici.arrayLayers   = 1;
        // V3-22：MSAA RT 的深度附件采样数必须与颜色一致
        ici.samples       = static_cast<VkSampleCountFlagBits>(rt.samples);
        ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
        ici.usage         = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(_device, &ici, nullptr, &rt.depthImage) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] RT depth vkCreateImage failed");
            if (rt.colorTex.valid()) destroyTexture(rt.colorTex);
            return {};
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(_device, rt.depthImage, &req);
        rt.depthMemAlloc = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);   // E1
        if (!rt.depthMemAlloc.valid()) {
            ENGINE_LOG_ERROR("[VK] RT depth memory alloc failed");
            vkDestroyImage(_device, rt.depthImage, nullptr);
            destroyTexture(rt.colorTex);
            return {};
        }
        rt.depthMemory = rt.depthMemAlloc.memory;
        if (vkBindImageMemory(_device, rt.depthImage, rt.depthMemory, rt.depthMemAlloc.offset) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] RT depth vkBindImageMemory failed at %s:%d", __FILE__, __LINE__);
            vkDestroyImage(_device, rt.depthImage, nullptr);
            _memAlloc.free(rt.depthMemAlloc);
            destroyTexture(rt.colorTex);
            return {};
        }

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image    = rt.depthImage;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format   = depthFmt;
        vci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(_device, &vci, nullptr, &rt.depthView) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] RT depth vkCreateImageView failed at %s:%d", __FILE__, __LINE__);
            vkDestroyImage(_device, rt.depthImage, nullptr);
            _memAlloc.free(rt.depthMemAlloc);
            destroyTexture(rt.colorTex);
            return {};
        }
    }

    RhiHandle base = _renderTargets.alloc(std::move(rt));
    RenderTargetHandle h; h.id = base.id; h.gen = base.gen;
    char name[48];
    std::snprintf(name, sizeof(name), "RHI_RenderTarget_%u", h.id);
    const VkTextureRes& ct = rt.colorTex.valid() ? textureRes(rt.colorTex) : VkTextureRes{};
    if (ct.image) {
        setObjName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(ct.image), name);
    }
    if (rt.depthImage) {
        setObjName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(rt.depthImage), name);
    }
    ENGINE_LOG_INFO("[VK] render target %ux%u created (id=%u, depth=%d)",
                    desc.width, desc.height, h.id, desc.hasDepth ? 1 : 0);
    return h;
}

void VulkanDevice::destroyRenderTarget(RenderTargetHandle h) {
    if (!_renderTargets.alive(h)) return;
    VkRenderTargetRes& rt = _renderTargets.get(h.id);
    if (rt.colorTex.valid()) destroyTexture(rt.colorTex);
    if (rt.depthTex.valid()) destroyTexture(rt.depthTex);
    if (rt.msaaColorImage || rt.msaaColorView) {
        VkImage img = rt.msaaColorImage;
        VkImageView view = rt.msaaColorView;
        VkMemAlloc mem = rt.msaaColorMem;
        _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                       [this, img, view, mem] {
                           if (view) vkDestroyImageView(_device, view, nullptr);
                           if (img)  vkDestroyImage(_device, img, nullptr);
                           if (mem.valid()) _memAlloc.free(mem);
                       });
    }
    if (rt.depthView || rt.depthImage) {
        VkImageView view = rt.depthView;
        VkImage img = rt.depthImage;
        VkMemAlloc mem = rt.depthMemAlloc;
        _delQueue.push(_frameCount + detail::DeletionQueue::kLagFrames,
                       [this, view, img, mem] {
                           if (view) vkDestroyImageView(_device, view, nullptr);
                           vkDestroyImage(_device, img, nullptr);
                           _memAlloc.free(mem);
                       });
    }
    _renderTargets.free(h.id);
}

TextureHandle VulkanDevice::renderTargetColorTexture(RenderTargetHandle h) const {
    if (!_renderTargets.alive(h)) return {};
    return _renderTargets.get(h.id).colorTex;
}

TextureHandle VulkanDevice::renderTargetDepthTexture(RenderTargetHandle h) const {
    if (!_renderTargets.alive(h)) return {};
    return _renderTargets.get(h.id).depthTex;
}

bool VulkanDevice::readbackTexture(TextureHandle tex, void* outData, size_t outSize,
                                   const ReadbackDesc& rb) {
    if (!_textures.alive(tex) || !outData) return false;
    const VkTextureRes& t = _textures.get(tex.id);
    if (!t.image || t.bpp == 0) return false;

    // 区域：宽高 0 = 整级（mip 级尺寸按 2^level 收缩）
    const uint32_t mipW = std::max(1u, t.width  >> rb.mipLevel);
    const uint32_t mipH = std::max(1u, t.height >> rb.mipLevel);
    const uint32_t w = rb.width  ? std::min(rb.width,  mipW) : mipW;
    const uint32_t h = rb.height ? std::min(rb.height, mipH) : mipH;
    const size_t bytes = static_cast<size_t>(w) * h * t.bpp;
    if (outSize < bytes) {
        ENGINE_LOG_WARN("[VK] readback: outSize %zu < %zu bytes", outSize, bytes);
        return false;
    }

    // 回读缓冲（HOST_VISIBLE，行距 0 = 紧凑；E1 子分配，页持久映射免 map）
    VkBuffer dst;
    VkMemAlloc dstMem;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size  = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VK_CHECK(vkCreateBuffer(_device, &bci, nullptr, &dst));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(_device, dst, &req);
    dstMem = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                 | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!dstMem.valid()) {
        ENGINE_LOG_ERROR("[VK] readback staging alloc failed");
        vkDestroyBuffer(_device, dst, nullptr);
        return false;
    }
    if (vkBindBufferMemory(_device, dst, dstMem.memory, dstMem.offset) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] readback vkBindBufferMemory failed at %s:%d", __FILE__, __LINE__);
        vkDestroyBuffer(_device, dst, nullptr);
        _memAlloc.free(dstMem);
        return false;
    }

    VkImageLayout curLayout = t.layout;   // lambda 内访问布局快照
    const VkImage image = t.image;
    submitOneTime([this, image, curLayout, dst, w, h, mip = rb.mipLevel, layer = rb.arrayLayer,
                   ox = rb.x, oy = rb.y, bytes](VkCommandBuffer cb) {
        // 当前布局（SHADER_READ_ONLY/UNDEFINED）→ TRANSFER_SRC
        VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toSrc.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc.oldLayout = curLayout;
        toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toSrc.image = image;
        toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toSrc);
        VkBufferImageCopy region{};
        region.bufferRowLength = 0;   // 紧凑存放
        // 关键：imageOffset 设为回读起点，否则总是从 (0,0) 复制（拾取坐标被忽略）
        region.imageOffset = {static_cast<int32_t>(ox), static_cast<int32_t>(oy), 0};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, mip, layer, 1};
        region.imageExtent = {w, h, 1};
        vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &region);
        // 回 SHADER_READ_ONLY（采样正常可用）
        VkImageMemoryBarrier toRead{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        toRead.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        toRead.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        toRead.image = image;
        toRead.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &toRead);
    }, [this, dst, dstMem, outData, bytes]() {
        // GPU 完成后回读（submitOneTime 的完成回调；E1：页持久映射直接拷）
        if (dstMem.mapped) {
            std::memcpy(outData, dstMem.mapped, bytes);
        }
        vkDestroyBuffer(_device, dst, nullptr);
        _memAlloc.free(dstMem);
    });
    // 布局跟踪回写（submitOneTime 已同步完成）
    _textures.get(tex.id).layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return true;
}

void VulkanDevice::recordRenderTargetBeginBarriers(VkCommandBuffer cb, RenderTargetHandle rt) {
    VkRenderTargetRes& r = _renderTargets.get(rt.id);
    if (r.colorTex.valid()) {
        VkTextureRes& t = _textures.get(r.colorTex.id);
        if (t.image && t.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
            VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            bar.srcAccessMask = (t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                    ? VK_ACCESS_SHADER_READ_BIT : 0;
            bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            bar.oldLayout = t.layout;
            bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = t.image;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cb,
                                 t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &bar);
            t.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
    }
    // V3-22：MSAA 颜色附件（每帧补同布局访问链；首帧从 UNDEFINED 转换）
    if (r.msaaColorImage) {
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask = (r.msaaColorLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                                ? 0 : VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        bar.oldLayout = r.msaaColorLayout;
        bar.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = r.msaaColorImage;
        bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &bar);
        r.msaaColorLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }

    // V3-17：可采样深度（阴影贴图）——SHADER_READ_ONLY / UNDEFINED → 深度附件
    if (r.depthTex.valid()) {
        VkTextureRes& t = _textures.get(r.depthTex.id);
        if (t.image && t.layout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            bar.srcAccessMask = (t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                                    ? VK_ACCESS_SHADER_READ_BIT : 0;
            bar.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                              | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            bar.oldLayout = t.layout;
            bar.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = t.image;
            bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cb,
                                 t.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                     ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &bar);
            t.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        }
    }

    if (r.depthImage && r.depthLayout != VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        // 首次使用：UNDEFINED → DEPTH_STENCIL_ATTACHMENT（loadOp 恒 CLEAR，内容本就丢弃）。
        // 注意用 1.0 时代的 DEPTH_STENCIL_ATTACHMENT_OPTIMAL：DEPTH_ATTACHMENT_OPTIMAL
        // 是 1.2 布局，需要 separate_depth_stencil_layouts 扩展（实例按 1.0 请求时不可用）
        VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        bar.srcAccessMask = 0;
        bar.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        bar.oldLayout = r.depthLayout;
        bar.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = r.depthImage;
        bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &bar);
        r.depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    }
}

void VulkanDevice::recordRenderTargetEndBarriers(VkCommandBuffer cb, RenderTargetHandle rt) {
    VkRenderTargetRes& r = _renderTargets.get(rt.id);

    // V3-17：可采样深度出 Pass → SHADER_READ_ONLY（阴影贴图随后被采样）
    if (r.depthTex.valid()) {
        VkTextureRes& dt = _textures.get(r.depthTex.id);
        if (dt.image && dt.layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
            VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            bar.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
            bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            bar.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = dt.image;
            bar.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &bar);
            dt.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }

    if (!r.colorTex.valid()) return;
    VkTextureRes& t = _textures.get(r.colorTex.id);
    if (!t.image || t.layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) return;
    VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    bar.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
    bar.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bar.image = t.image;
    bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    // dstAccess 含 TRANSFER_READ（回读就绪），stage 掩码必须同时覆盖 TRANSFER
    //（验证层：dstAccessMask 不受 dstStageMask 支持即报 error）
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                         | VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &bar);
    t.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

UniformBlockHandle VulkanDevice::createUniformBlock(uint32_t size) {
    // V3-2：不再分配 UBO/描述符集。数据统一进帧内环（见 allocDrawUniforms），
    // 句柄只保留"这是一次绘制数据块"的身份和尺寸，业务侧调用方式不变。
    VkUniformBlockRes blk;
    blk.size = size;
    RhiHandle base = _uniformBlocks.alloc(blk);
    UniformBlockHandle h; h.id = base.id; h.gen = base.gen;
    return h;
}

void VulkanDevice::bindInstanceVertexBuffer(VkCommandBuffer cb, BufferHandle h) const {
    if (!_buffers.alive(h)) return;
    VkBuffer vb = _buffers.get(h.id).buffer;
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cb, 1, 1, &vb, &offset);   // binding1 = 实例率
}

} // namespace rhi
