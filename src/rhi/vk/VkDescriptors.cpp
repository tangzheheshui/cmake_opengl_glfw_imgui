//
//  VkDescriptors.cpp
//
#include "VkDescriptors.h"
#include <cstring>

namespace rhi {

bool VkDescriptors::init(VkDevice device, uint32_t minUboAlign,
                         VkMemoryAllocator* alloc, NameFn nameFn) {
    _device = device;
    _alloc  = alloc;
    _nameFn = std::move(nameFn);
    if (!_nameFn) _nameFn = [](VkObjectType, uint64_t, const char*) {};
    _minAlign = minUboAlign;

    createPoolsAndLayouts();

    // 环初始就留出 offset 0 的零块：命令列表构造期就能拿到有效偏移
    static const uint8_t kZeros[kDrawUniformsSize] = {};
    const uint32_t zeroOff = _ring.alloc(kZeros, kDrawUniformsSize);
    _defaultUniformOffset = (zeroOff == UINT32_MAX) ? 0 : zeroOff;

    return _ring.valid();
}

void VkDescriptors::createPoolsAndLayouts() {
    // ---- set0：帧 uniform 环。动态偏移 = 每绘制的小数据块位置 ----
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

    // ---- set1..4：纹理槽。每个槽一个 set，每张纹理创建一个（写一次）----
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

    // ---- 帧 set 的池（1 个 set，永不复位，只重写）----
    VkDescriptorPoolSize framePoolSize{};
    framePoolSize.type            = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    framePoolSize.descriptorCount = 1;
    VkDescriptorPoolCreateInfo fpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    fpi.maxSets       = 1;
    fpi.poolSizeCount = 1;
    fpi.pPoolSizes    = &framePoolSize;
    VK_CHECK(vkCreateDescriptorPool(_device, &fpi, nullptr, &_frameDescPool));
    _nameFn(VK_OBJECT_TYPE_DESCRIPTOR_POOL,
            reinterpret_cast<uint64_t>(_frameDescPool), "RHI_FrameDescriptorPool");

    // ---- 全局管线布局：set0 + 4 个纹理槽 + 固定 push constant ----
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset     = 0;
    pushRange.size       = kPushTotalSize;
    VkDescriptorSetLayout setLayouts[1 + kMaxTexSlots];
    setLayouts[0] = _frameSetLayout;
    for (uint32_t i = 0; i < kMaxTexSlots; ++i) setLayouts[i + 1] = _texSlotSetLayout;
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges    = &pushRange;
    plci.setLayoutCount         = 1 + kMaxTexSlots;
    plci.pSetLayouts            = setLayouts;
    VK_CHECK(vkCreatePipelineLayout(_device, &plci, nullptr, &_pipelineLayout));
    _nameFn(VK_OBJECT_TYPE_PIPELINE_LAYOUT,
            reinterpret_cast<uint64_t>(_pipelineLayout), "RHI_PipelineLayout");

    // ---- 帧 uniform 环（默认 1MB；对齐取设备要求与 256 的较大值）----
    const uint32_t align = _minAlign < 256 ? 256 : _minAlign;
    if (!_ring.init(_device, _alloc, 1024u * 1024u, align)) {
        ENGINE_LOG_ERROR("[VK] frame uniform ring init failed");
    }
    writeFrameSet();
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

void VkDescriptors::writeFrameSet() {
    if (!_ring.valid()) return;
    if (!_frameSet) {
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool     = _frameDescPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts        = &_frameSetLayout;
        VK_CHECK(vkAllocateDescriptorSets(_device, &dai, &_frameSet));
        _nameFn(VK_OBJECT_TYPE_DESCRIPTOR_SET,
                reinterpret_cast<uint64_t>(_frameSet), "RHI_FrameUniformSet");
    }
    VkDescriptorBufferInfo bi{_ring.buffer(), 0, kDrawUniformsSize};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet          = _frameSet;
    w.dstBinding      = 0;
    w.descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w.descriptorCount = 1;
    w.pBufferInfo     = &bi;
    vkUpdateDescriptorSets(_device, 1, &w, 0, nullptr);
}

void VkDescriptors::beginFrame() {
    _ring.reset();
    static const uint8_t kZeros[kDrawUniformsSize] = {};
    const uint32_t off = _ring.alloc(kZeros, kDrawUniformsSize);
    _defaultUniformOffset = (off == UINT32_MAX) ? 0 : off;
}

uint32_t VkDescriptors::allocDrawUniforms(const void* data, uint32_t size) {
    if (!data || size == 0) return UINT32_MAX;
    // 尾部清零：块尾可能残留上一块的数据，着色器读到的必须是确定值
    static thread_local uint8_t block[kDrawUniformsSize];
    const uint32_t n = size < kDrawUniformsSize ? size : kDrawUniformsSize;
    std::memcpy(block, data, n);
    if (n < kDrawUniformsSize) std::memset(block + n, 0, kDrawUniformsSize - n);

    uint32_t off = _ring.alloc(block, kDrawUniformsSize);
    if (off == UINT32_MAX) {
        growRing();
        off = _ring.alloc(block, kDrawUniformsSize);
    }
    return off;
}

void VkDescriptors::growRing() {
    if (!_ring.grow(_ring.capacity() * 2)) {
        ENGINE_LOG_ERROR("[VK] frame uniform ring grow failed");
        return;
    }
    writeFrameSet();   // buffer 换了，描述符要重写
}

void VkDescriptors::shutdown() {
    _ring.shutdown();
    for (auto pool : _texPools) {
        if (pool) vkDestroyDescriptorPool(_device, pool, nullptr);
    }
    _texPools.clear();
    if (_frameDescPool)    vkDestroyDescriptorPool(_device, _frameDescPool, nullptr);
    if (_pipelineLayout)   vkDestroyPipelineLayout(_device, _pipelineLayout, nullptr);
    if (_frameSetLayout)   vkDestroyDescriptorSetLayout(_device, _frameSetLayout, nullptr);
    if (_texSlotSetLayout) vkDestroyDescriptorSetLayout(_device, _texSlotSetLayout, nullptr);
    _frameDescPool = VK_NULL_HANDLE;
    _pipelineLayout = VK_NULL_HANDLE;
    _frameSetLayout = VK_NULL_HANDLE;
    _texSlotSetLayout = VK_NULL_HANDLE;
    _frameSet = VK_NULL_HANDLE;
}

} // namespace rhi
