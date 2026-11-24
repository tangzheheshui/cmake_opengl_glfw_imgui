//
//  VkDescriptors.cpp
//
#include "VkDescriptors.h"
#include <cstring>

namespace rhi {

bool VkDescriptors::init(VkDevice device, uint32_t minUboAlign,
                         VkMemoryAllocator* alloc, uint32_t texSlotCount, NameFn nameFn) {
    _device = device;
    _alloc  = alloc;
    _nameFn = std::move(nameFn);
    if (!_nameFn) _nameFn = [](VkObjectType, uint64_t, const char*) {};
    _minAlign = minUboAlign;
    if (texSlotCount < 1) texSlotCount = 1;
    if (texSlotCount > kMaxTexSlots) texSlotCount = kMaxTexSlots;
    _texSlotCount = texSlotCount;

    createPoolsAndLayouts(texSlotCount);
    return true;
}

void VkDescriptors::createPoolsAndLayouts(uint32_t texSlotCount) {
    // ---- set0：帧数据（动态偏移 UBO）----
    VkDescriptorSetLayoutBinding frameBinding{};
    frameBinding.binding         = 0;
    frameBinding.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    frameBinding.descriptorCount = 1;
    frameBinding.stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo fli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    fli.bindingCount = 1;
    fli.pBindings    = &frameBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(_device, &fli, nullptr, &_frameSetLayout));
    _nameFn(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
            reinterpret_cast<uint64_t>(_frameSetLayout), "RHI_FrameSetLayout");

    // ---- set1..N：纹理槽。每个槽一个 set，每张纹理创建一个（写一次）----
    VkDescriptorSetLayoutBinding texBinding{};
    texBinding.binding         = 0;
    texBinding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texBinding.descriptorCount = 1;
    texBinding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo tli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tli.bindingCount = 1;
    tli.pBindings    = &texBinding;
    VK_CHECK(vkCreateDescriptorSetLayout(_device, &tli, nullptr, &_texSlotSetLayout));
    _nameFn(VK_OBJECT_TYPE_DESCRIPTOR_SET_LAYOUT,
            reinterpret_cast<uint64_t>(_texSlotSetLayout), "RHI_TexSlotSetLayout");

    growTexturePool();
    growFramePool();

    // ---- 全局管线布局：set0 + N 个纹理槽 + 固定 push constant ----
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = kPushTotalSize;
    VkDescriptorSetLayout setLayouts[1 + kMaxTexSlots];
    setLayouts[0] = _frameSetLayout;
    for (uint32_t i = 0; i < texSlotCount; ++i) setLayouts[i + 1] = _texSlotSetLayout;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pushRange;
    plci.setLayoutCount         = 1 + texSlotCount;
    plci.pSetLayouts            = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(_device, &plci, nullptr, &_pipelineLayout));
    _nameFn(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
            reinterpret_cast<uint64_t>(_pipelineLayout), "RHI_PipelineLayout");
}

void VkDescriptors::growTexturePool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = _poolCapSampler;
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets       = _poolCapSets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &poolSize;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(_device, &pi, nullptr, &pool));
    _texPools.push_back(pool);
    char name[48];
    std::snprintf(name, sizeof(name), "RHI_TexDescriptorPool_%zu", _texPools.size() - 1);
    _nameFn(VK_OBJECT_TYPE_DESCRIPTOR_POOL, reinterpret_cast<uint64_t>(pool), name);
    ENGINE_LOG_INFO("[VK] descriptor pool #%zu: sets=%u sampler=%u",
                    _texPools.size() - 1, _poolCapSets, _poolCapSampler);
}

void VkDescriptors::growFramePool() {
    VkDescriptorPoolSize poolSize{};
    poolSize.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSize.descriptorCount = _frameCapSets;
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets       = _frameCapSets;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &poolSize;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VK_CHECK(vkCreateDescriptorPool(_device, &pi, nullptr, &pool));
    _framePools.push_back(pool);
    char name[48];
    std::snprintf(name, sizeof(name), "RHI_FrameDescriptorPool_%zu", _framePools.size() - 1);
    _nameFn(VK_OBJECT_TYPE_DESCRIPTOR_POOL, reinterpret_cast<uint64_t>(pool), name);
}

bool VkDescriptors::createFrameSet(VkBuffer ringBuffer, uint32_t tag, VkDescriptorSet& outSet) {
    outSet = VK_NULL_HANDLE;
    if (ringBuffer == VK_NULL_HANDLE) return false;
    if (_framePools.empty()) growFramePool();

    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool     = _framePools.back();
    dai.descriptorSetCount = 1;
    dai.pSetLayouts        = &_frameSetLayout;
    VkResult r = vkAllocateDescriptorSets(_device, &dai, &outSet);
    if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL) {
        growFramePool();
        dai.descriptorPool = _framePools.back();
        r = vkAllocateDescriptorSets(_device, &dai, &outSet);
    }
    if (r != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] frame descriptor set alloc failed (%d)", static_cast<int>(r));
        outSet = VK_NULL_HANDLE;
        return false;
    }

    VkDescriptorBufferInfo bi{ringBuffer, 0, kDrawUniformsSize};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = outSet;
    w.dstBinding      = 0;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w.descriptorCount = 1;
    w.pBufferInfo     = &bi;
    vkUpdateDescriptorSets(_device, 1, &w, 0, nullptr);

    char name[48];
    std::snprintf(name, sizeof(name), "RHI_FrameSet_%u", tag);
    _nameFn(VK_OBJECT_TYPE_DESCRIPTOR_SET, reinterpret_cast<uint64_t>(outSet), name);
    return true;
}

bool VkDescriptors::createTextureSet(VkSampler sampler, VkImageView view,
                                     VkDescriptorSet& outSet) {
    outSet = VK_NULL_HANDLE;
    if (_texPools.empty()) growTexturePool();
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool     = _texPools.back();
    dai.descriptorSetCount = 1;
    dai.pSetLayouts        = &_texSlotSetLayout;
    VkResult r = vkAllocateDescriptorSets(_device, &dai, &outSet);
    if (r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL) {
        growTexturePool();
        dai.descriptorPool = _texPools.back();
        r = vkAllocateDescriptorSets(_device, &dai, &outSet);
    }
    if (r != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] texture descriptor set alloc failed (%d)", static_cast<int>(r));
        outSet = VK_NULL_HANDLE;
        return false;
    }
    VkDescriptorImageInfo ii{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = outSet;
    w.dstBinding      = 0;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.descriptorCount = 1;
    w.pImageInfo      = &ii;
    vkUpdateDescriptorSets(_device, 1, &w, 0, nullptr);
    return true;
}

void VkDescriptors::shutdown() {
    for (auto pool : _framePools) {
        if (pool) vkDestroyDescriptorPool(_device, pool, nullptr);
    }
    _framePools.clear();
    for (auto pool : _texPools) {
        if (pool) vkDestroyDescriptorPool(_device, pool, nullptr);
    }
    _texPools.clear();
    if (_pipelineLayout)   vkDestroyPipelineLayout(_device, _pipelineLayout, nullptr);
    if (_frameSetLayout)   vkDestroyDescriptorSetLayout(_device, _frameSetLayout, nullptr);
    if (_texSlotSetLayout) vkDestroyDescriptorSetLayout(_device, _texSlotSetLayout, nullptr);
    _pipelineLayout = VK_NULL_HANDLE;
    _frameSetLayout = VK_NULL_HANDLE;
    _texSlotSetLayout = VK_NULL_HANDLE;
}

} // namespace rhi
