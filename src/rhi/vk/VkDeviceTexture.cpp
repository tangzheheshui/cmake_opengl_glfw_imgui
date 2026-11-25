//
//  VkDeviceTexture.cpp —— 纹理创建与上传
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

static uint32_t fullMipChainLevels(uint32_t w, uint32_t h) {
    uint32_t m = std::max(w, h);
    uint32_t levels = 1;
    while (m > 1) { m >>= 1; ++levels; }
    return levels;
}

static bool formatBlitSupported(VkPhysicalDevice pd, VkFormat f) {
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(pd, f, &p);
    return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) != 0
        && (p.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT) != 0;
}

TextureHandle VulkanDevice::createTexture(const TextureDesc& desc) {
    if (_deviceLost) return {};
    VkTextureRes t;
    t.width = desc.width; t.height = desc.height;
    const uint32_t layerCount = desc.cubemap ? 6u : 1u;
    const FormatInfo& fi = FormatInfo::of(desc.format);
    const VkFormat vkFmt = static_cast<VkFormat>(fi.vkFormat());
    const uint32_t bpp = fi.bytesPerPixel();
    if (bpp == 0) {
        ENGINE_LOG_ERROR("[VK] unsupported format bpp (%d)", static_cast<int>(desc.format));
        return {};
    }
    // mip 级数：mipLevels==0 = 完整链；generateMips=true 且未显式给多级时
    // 也按完整链分配（B3c：此前 generateMips 单独使用时只分配 1 级，天空盒
    // 的 mip 链实际没有生成）
    uint32_t mipLevels = desc.mipLevels > 0 ? desc.mipLevels : 1;
    bool generateMips = desc.generateMips && desc.initialData;
    if (generateMips && mipLevels <= 1) {
        mipLevels = fullMipChainLevels(desc.width, desc.height);
    }
    if (generateMips && mipLevels > 1 && !formatBlitSupported(_physicalDevice, vkFmt)) {
        ENGINE_LOG_WARN_ONCE("[VK] blit unsupported for format %d, mipmap generation disabled",
                             static_cast<int>(vkFmt));
        generateMips = false;
        mipLevels = desc.mipLevels > 0 ? desc.mipLevels : 1;
    }

    // ---- image（DEVICE_LOCAL + OPTIMAL）----
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType     = VK_IMAGE_TYPE_2D;
    ici.format        = vkFmt;
    ici.extent        = {desc.width, desc.height, 1};
    ici.mipLevels     = mipLevels;
    ici.arrayLayers   = layerCount;
    ici.samples       = VK_SAMPLE_COUNT_1_BIT;
    if (desc.cubemap) ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    ici.tiling        = VK_IMAGE_TILING_OPTIMAL;
    // B4：TRANSFER_SRC 恒开（readbackTexture 回读）；COLOR_ATTACHMENT 按 desc
    const bool isDepth = fi.isDepth();
    ici.usage         = VK_IMAGE_USAGE_SAMPLED_BIT
                      | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                      | (desc.initialData ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : 0)
                      | (generateMips ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0)
                      | ((desc.usage & TEX_COLOR_ATTACHMENT) ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0)
                      // V3-17：深度附件（阴影贴图）。采样与附件位可以同时存在
                      | ((desc.usage & TEX_DEPTH_ATTACHMENT) ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0);
    if (isDepth) {
        // 深度图不做颜色上传/mip 生成
        ici.usage &= ~static_cast<VkImageUsageFlags>(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        generateMips = false;
        mipLevels = 1;
    }
    ici.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VK_CHECK_RET(vkCreateImage(_device, &ici, nullptr, &t.image));
    t.bpp = bpp;   // B4：readback 区域字节数计算用

    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(_device, t.image, &req);
    t.memAlloc = _memAlloc.alloc(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);   // E1
    if (!t.memAlloc.valid()) {
        ENGINE_LOG_ERROR("[VK] texture memory alloc failed (%ux%u)", desc.width, desc.height);
        vkDestroyImage(_device, t.image, nullptr);
        return {};
    }
    t.memory = t.memAlloc.memory;
    if (vkBindImageMemory(_device, t.image, t.memory, t.memAlloc.offset) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] vkBindImageMemory failed at %s:%d", __FILE__, __LINE__);
        vkDestroyImage(_device, t.image, nullptr);
        _memAlloc.free(t.memAlloc);
        return {};
    }

    // ---- 数据上传：staging → 拷贝 + （可选）mipmap blit + 布局转换 ----
    // 可能在 render pass 活跃的帧内被调用：GPU 工作经 deferGpuWork 延迟到下一帧
    // 开头执行；数据由 lambda 持有（调用方的 initialData 生命周期未知）
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(desc.width) * desc.height * bpp * layerCount;
    // E2：上传一律走延迟批处理（下一次 beginFrame 统一提交），布局由上传
    // lambda 完成后回写；在那之前 textureSet() 返回空，绘制退白纹理兜底。
    if (desc.initialData && bytes > 0) {
        auto dataCopy = std::make_shared<std::vector<unsigned char>>(
            static_cast<const unsigned char*>(desc.initialData),
            static_cast<const unsigned char*>(desc.initialData) + static_cast<size_t>(bytes));
        // staging 在宿主阶段创建并填充（宿主操作可安全执行于帧内）；
        // 拷贝命令延迟录制；staging 的销毁在 GPU 执行完成后由完成回调执行。
        // E1：HOST_VISIBLE 子分配（页持久映射，免 map/unmap）
        VkBuffer staging;
        VkMemAlloc stagingMem;
        VkBufferCreateInfo sbi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        sbi.size  = bytes;
        sbi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        sbi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(_device, &sbi, nullptr, &staging) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] staging vkCreateBuffer failed (%llu bytes)",
                             static_cast<unsigned long long>(bytes));
            vkDestroyImage(_device, t.image, nullptr);
            _memAlloc.free(t.memAlloc);
            return {};
        }
        VkMemoryRequirements req2;
        vkGetBufferMemoryRequirements(_device, staging, &req2);
        stagingMem = _memAlloc.alloc(req2, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                                         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (!stagingMem.valid()) {
            ENGINE_LOG_ERROR("[VK] staging alloc failed (%llu bytes)",
                             static_cast<unsigned long long>(bytes));
            vkDestroyBuffer(_device, staging, nullptr);
            vkDestroyImage(_device, t.image, nullptr);
            _memAlloc.free(t.memAlloc);
            return {};
        }
        if (vkBindBufferMemory(_device, staging, stagingMem.memory, stagingMem.offset) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] staging vkBindBufferMemory failed at %s:%d", __FILE__, __LINE__);
            vkDestroyBuffer(_device, staging, nullptr);
            _memAlloc.free(stagingMem);
            vkDestroyImage(_device, t.image, nullptr);
            _memAlloc.free(t.memAlloc);
            return {};
        }
        void* mapped = stagingMem.mapped;
        std::memcpy(mapped, dataCopy->data(), static_cast<size_t>(bytes));

        // V3-13：槽位启用复用后不能再预测 id（原写法 = size()+1）。
        // 用一个句柄占位对象：纹理分配完成后回填，延迟回调按句柄校验后再写布局。
        auto texHandle = std::make_shared<TextureHandle>();
        _deferredTexHandles.push_back(texHandle);

        deferGpuWork([this, t, staging, stagingMem, w = desc.width, h = desc.height,
                      bpp, layerCount, mipLevels, generateMips, texHandle](VkCommandBuffer cb) {

            VkImageMemoryBarrier bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            bar.srcAccessMask = 0;
            bar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            bar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            bar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            bar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bar.image = t.image;
            bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layerCount};
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &bar);
            std::vector<VkBufferImageCopy> regions(layerCount);
            for (uint32_t face = 0; face < layerCount; ++face) {
                regions[face] = {};
                regions[face].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, face, 1};
                regions[face].imageExtent = {w, h, 1};
                regions[face].bufferOffset = static_cast<VkDeviceSize>(face) * w * h * (bpp);
            }
            vkCmdCopyBufferToImage(cb, staging, t.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   layerCount, regions.data());

            if (generateMips && mipLevels > 1) {
                // B3：逐级 blit 生成 mip 链（每层独立；第 i-1 级 DST→SRC 后作源，
                // 双线性缩半写入第 i 级）。不支持的格式已在创建期回退单级。
                for (uint32_t layer = 0; layer < layerCount; ++layer) {
                    for (uint32_t i = 1; i < mipLevels; ++i) {
                        VkImageMemoryBarrier srcBar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        srcBar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        srcBar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                        srcBar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        srcBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                        srcBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        srcBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        srcBar.image = t.image;
                        srcBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, layer, 1};
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                             0, nullptr, 0, nullptr, 1, &srcBar);

                        // 目标级此前从未使用（UNDEFINED），先转为 TRANSFER_DST。
                        // B3c：mip 链此前因 mipLevels 恒为 1 从未真正生成，
                        // 该缺失屏障未暴露；启用完整链后被验证层捕获。
                        VkImageMemoryBarrier dstBar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                        dstBar.srcAccessMask = 0;
                        dstBar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                        dstBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        dstBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                        dstBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        dstBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                        dstBar.image = t.image;
                        dstBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, i, 1, layer, 1};
                        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                             0, nullptr, 0, nullptr, 1, &dstBar);

                        VkImageBlit blit{};
                        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, layer, 1};
                        blit.srcOffsets[1] = { static_cast<int32_t>(w >> (i - 1)),
                                               static_cast<int32_t>(h >> (i - 1)), 1 };
                        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, layer, 1};
                        blit.dstOffsets[1]  = { static_cast<int32_t>(std::max(1u, w >> i)),
                                                static_cast<int32_t>(std::max(1u, h >> i)), 1 };
                        vkCmdBlitImage(cb, t.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       1, &blit, VK_FILTER_LINEAR);
                    }
                }
                // 全链 → SHADER_READ：末级在 DST，其余在 SRC，分两段转换
                bar.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
                bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mipLevels - 1, 0, layerCount};
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &bar);
                bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, mipLevels - 1, 1, 0, layerCount};
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &bar);
            } else {
                bar.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                bar.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                bar.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                bar.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                                     0, nullptr, 0, nullptr, 1, &bar);
            }
            // B4：布局跟踪（上传完成的稳定布局；附件/回读转换以此为基）
            if (texHandle->valid() && _textures.alive(*texHandle)) {
                _textures.get(texHandle->id).layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }, [this, staging, stagingMem]() {
            vkDestroyBuffer(_device, staging, nullptr);
            _memAlloc.free(stagingMem);   // E1
        });
    }
    // ---- view + sampler ----
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image    = t.image;
    vci.viewType = desc.cubemap ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    vci.format   = vkFmt;
    // V3-17：深度格式的视图用 DEPTH 切面
    vci.subresourceRange = {static_cast<VkImageAspectFlags>(
                                isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT),
                            0, mipLevels, 0, layerCount};
    if (vkCreateImageView(_device, &vci, nullptr, &t.view) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] vkCreateImageView failed at %s:%d", __FILE__, __LINE__);
        vkDestroyImage(_device, t.image, nullptr);
        _memAlloc.free(t.memAlloc);
        return {};
    }

    // B3：采样参数由 TextureDesc::sampler 驱动（契约 SamplerDesc）
    const SamplerDesc& sd = desc.sampler;
    auto filterOf = [](FilterMode m) {
        return m == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    };
    auto wrapOf = [](WrapMode m) {
        switch (m) {
            case WrapMode::Repeat:      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
            case WrapMode::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            case WrapMode::Mirror:      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter        = filterOf(sd.magFilter);
    sci.minFilter        = filterOf(sd.minFilter);
    sci.mipmapMode       = sd.mipFilter == FilterMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                                               : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    // cubemap：Vulkan 要求地址模式为 CLAMP_TO_EDGE/BORDER/MIRRORED_CLAMP
    // （REPEAT 采样 cube 属违规），与 GL 天空盒路径的 CLAMP_TO_EDGE 对齐
    sci.addressModeU     = desc.cubemap ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : wrapOf(sd.wrapU);
    sci.addressModeV     = desc.cubemap ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : wrapOf(sd.wrapV);
    sci.addressModeW     = desc.cubemap ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : wrapOf(sd.wrapW);
    sci.mipLodBias       = 0.0f;
    sci.anisotropyEnable = VK_FALSE;
    sci.maxAnisotropy    = 1.0f;
    if (sd.maxAnisotropy > 1.0f && _caps.features.samplerAnisotropy == VK_TRUE) {
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy    = std::min(sd.maxAnisotropy, _caps.props.limits.maxSamplerAnisotropy);
    }
    sci.compareEnable    = sd.compareEnable ? VK_TRUE : VK_FALSE;
    // 深度比较采样（阴影 PCF 预留）：Reversed-Z 时代取 GREATER
    sci.compareOp        = VK_COMPARE_OP_GREATER;
    sci.minLod           = sd.minLod;
    sci.maxLod           = sd.maxLod;
    sci.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    if (vkCreateSampler(_device, &sci, nullptr, &t.sampler) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] vkCreateSampler failed at %s:%d", __FILE__, __LINE__);
        vkDestroyImageView(_device, t.view, nullptr);
        vkDestroyImage(_device, t.image, nullptr);
        _memAlloc.free(t.memAlloc);
        return {};
    }

    // V3-2：纹理自带描述符集（写一次）。绘制期只绑 set，不再 vkUpdateDescriptorSets。
    {
        VkDescriptorSet texSet = VK_NULL_HANDLE;
        if (!_descs.createTextureSet(t.sampler, t.view, texSet)) {
            vkDestroySampler(_device, t.sampler, nullptr);
            vkDestroyImageView(_device, t.view, nullptr);
            vkDestroyImage(_device, t.image, nullptr);
            _memAlloc.free(t.memAlloc);
            return {};
        }
        t.set = texSet;
    }

    RhiHandle base = _textures.alloc(std::move(t));
    TextureHandle h; h.id = base.id; h.gen = base.gen;
    // 回填延迟上传回调里的句柄占位（槽位复用也安全：句柄带 gen）
    if (!_deferredTexHandles.empty() && !_deferredTexHandles.back()->valid()) {
        *_deferredTexHandles.back() = h;
    }
    char tname[48];
    std::snprintf(tname, sizeof(tname), "RHI_Texture_%u", h.id);
    setObjName(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(t.image), tname);
    setObjName(VK_OBJECT_TYPE_IMAGE_VIEW, reinterpret_cast<uint64_t>(t.view), tname);
    setObjName(VK_OBJECT_TYPE_SAMPLER, reinterpret_cast<uint64_t>(t.sampler), tname);
    return h;
}

} // namespace rhi
