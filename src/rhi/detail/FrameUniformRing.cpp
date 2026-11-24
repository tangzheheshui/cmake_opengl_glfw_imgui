//
//  FrameUniformRing.cpp
//
#include "FrameUniformRing.h"
#include "log/LogMacros.h"
#include <cstring>

namespace rhi {
namespace detail {

static uint32_t alignUp(uint32_t v, uint32_t a) {
    return a ? ((v + a - 1) / a) * a : v;
}

bool FrameUniformRing::init(VkDevice device, VkMemoryAllocator* allocator,
                            uint32_t bytes, uint32_t alignment) {
    _device = device;
    _alloc  = allocator;
    _align  = alignment < 1 ? 1 : alignment;
    return createBuffer(bytes);
}

bool FrameUniformRing::createBuffer(uint32_t bytes) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size        = bytes;
    bi.usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(_device, &bi, nullptr, &_buffer) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] uniform ring vkCreateBuffer failed (%u bytes)", bytes);
        _buffer = VK_NULL_HANDLE;
        return false;
    }

    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(_device, _buffer, &req);
    _mem = _alloc->alloc(req, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                              | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!_mem.valid() || !_mem.mapped) {
        ENGINE_LOG_ERROR("[VK] uniform ring memory alloc failed (%u bytes)", bytes);
        vkDestroyBuffer(_device, _buffer, nullptr);
        _buffer = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindBufferMemory(_device, _buffer, _mem.memory, _mem.offset) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK] uniform ring vkBindBufferMemory failed");
        _alloc->free(_mem);
        _mem = VkMemAlloc{};
        vkDestroyBuffer(_device, _buffer, nullptr);
        _buffer = VK_NULL_HANDLE;
        return false;
    }

    _mapped   = _mem.mapped;
    _capacity = bytes;
    _cursor   = 0;
    return true;
}

void FrameUniformRing::destroyBuffer() {
    if (_buffer) {
        vkDestroyBuffer(_device, _buffer, nullptr);
        _buffer = VK_NULL_HANDLE;
    }
    if (_mem.valid() && _alloc) {
        _alloc->free(_mem);
    }
    _mem      = VkMemAlloc{};
    _mapped   = nullptr;
    _capacity = 0;
    _cursor   = 0;
}

void FrameUniformRing::shutdown() {
    destroyBuffer();
    _device = VK_NULL_HANDLE;
    _alloc  = nullptr;
}

uint32_t FrameUniformRing::alloc(const void* data, uint32_t size) {
    if (!_buffer || !data || size == 0) return UINT32_MAX;
    const uint32_t offset = alignUp(_cursor, _align);
    if (offset >= _capacity || size > _capacity - offset) return UINT32_MAX;
    std::memcpy(static_cast<uint8_t*>(_mapped) + offset, data, size);
    _cursor = offset + size;
    return offset;
}

bool FrameUniformRing::grow(uint32_t minBytes) {
    uint32_t newCap = _capacity ? _capacity : (256u * 1024u);
    while (newCap < minBytes) newCap *= 2;
    destroyBuffer();
    if (!createBuffer(newCap)) return false;
    ENGINE_LOG_INFO("[VK] uniform ring grown to %u bytes", newCap);
    return true;
}

} // namespace detail
} // namespace rhi
