//
//  VkMemoryAllocator.cpp —— E1: VkDeviceMemory 子分配器（见头文件说明）
//

#include "log/LogMacros.h"
#include "VkMemoryAllocator.h"

#include <algorithm>
#include <cstring>

namespace rhi {

void VkMemoryAllocator::init(VkPhysicalDevice pd, VkDevice device) {
    _pd = pd;
    _device = device;
}

void VkMemoryAllocator::shutdown() {
    if (_liveCount != 0) {
        ENGINE_LOG_WARN("[VK][mem] shutdown with %zu live allocations (leak)",
                        _liveCount);
    }
    for (auto& p : _pages) {
        if (p.mapped) {
            vkUnmapMemory(_device, p.memory);
        }
        if (p.memory) {
            vkFreeMemory(_device, p.memory, nullptr);
        }
    }
    for (auto& d : _dedicated) {
        if (d.mapped) {
            vkUnmapMemory(_device, d.memory);
        }
        if (d.memory) {
            vkFreeMemory(_device, d.memory, nullptr);
        }
    }
    ENGINE_LOG_INFO("[VK][mem] shutdown: pages=%zu dedicated=%zu live=%zu",
                    _pages.size(), _dedicated.size(), _liveCount);
    _pages.clear();
    _dedicated.clear();
    _liveCount = 0;
}

uint32_t VkMemoryAllocator::resolveMemoryType(uint32_t typeBits,
                                              VkMemoryPropertyFlags props) const {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(_pd, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeBits & (1u << i)) != 0 &&
            (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0xFFFFFFFFu;
}

VkMemAlloc VkMemoryAllocator::alloc(const VkMemoryRequirements& req,
                                    VkMemoryPropertyFlags props) {
    const uint32_t type = resolveMemoryType(req.memoryTypeBits, props);
    if (type == 0xFFFFFFFFu) {
        ENGINE_LOG_ERROR("[VK][mem] no memory type for props=0x%x",
                         static_cast<unsigned>(props));
        return {};
    }
    const bool hostVisible = (props & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
    // 大块走专用；小块（≤ 页一半）走页内子分配
    if (req.size >= kDedicatedThreshold) {
        return allocDedicated(type, req, hostVisible);
    }
    return allocFromPage(type, req, hostVisible);
}

VkMemAlloc VkMemoryAllocator::allocDedicated(uint32_t memTypeIndex,
                                             const VkMemoryRequirements& req,
                                             bool hostVisible) {
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = req.size;
    mai.memoryTypeIndex = memTypeIndex;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    if (vkAllocateMemory(_device, &mai, nullptr, &mem) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK][mem] dedicated alloc %llu bytes failed",
                         static_cast<unsigned long long>(req.size));
        return {};
    }
    VkMemAlloc a;
    a.memory  = mem;
    a.offset  = 0;
    a.size    = req.size;
    if (hostVisible) {
        if (vkMapMemory(_device, mem, 0, req.size, 0, &a.mapped) != VK_SUCCESS) {
            a.mapped = nullptr;
        }
    }
    _dedicated.push_back({mem, a.mapped, req.size});
    ++_liveCount;
    return a;
}

VkMemAlloc VkMemoryAllocator::allocFromPage(uint32_t memTypeIndex,
                                            const VkMemoryRequirements& req,
                                            bool hostVisible) {
    const VkDeviceSize aligned =
        (req.size + req.alignment - 1) / req.alignment * req.alignment;

    // 1) 现有同类型页里找 first-fit 空闲块
    for (size_t i = 0; i < _pages.size(); ++i) {
        Page& p = _pages[i];
        if (p.memTypeIndex != memTypeIndex) continue;
        for (size_t b = 0; b < p.freeList.size(); ++b) {
            FreeBlock& fb = p.freeList[b];
            const VkDeviceSize alignedOff =
                (fb.offset + req.alignment - 1) / req.alignment * req.alignment;
            const VkDeviceSize pad = alignedOff - fb.offset;
            if (pad + aligned > fb.size) continue;

            VkMemAlloc a;
            a.memory  = p.memory;
            a.offset  = alignedOff;
            a.mapped  = p.mapped
                            ? static_cast<char*>(p.mapped) + static_cast<size_t>(alignedOff)
                            : nullptr;
            a.pageMem = p.memory;   // 页身份 = memory 句柄（下标会因页回收错位）
            a.size    = req.size;

            // 块拆分：头部 padding 归还空闲链，剩余尾部收缩
            if (pad > 0) {
                FreeBlock head{fb.offset, pad};
                fb.offset = alignedOff;
                fb.size  -= pad;
                p.freeList.insert(p.freeList.begin() + b, head);
                ++b;   // fb 现在在 b+1
            }
            if (fb.size > aligned) {
                FreeBlock rest{fb.offset + aligned, fb.size - aligned};
                fb.size = aligned;
                p.freeList.insert(p.freeList.begin() + b + 1, rest);
            }
            p.freeList.erase(p.freeList.begin() + b);
            ++p.refCount;
            ++_liveCount;
            return a;
        }
    }

    // 2) 开新页
    Page p;
    p.memTypeIndex = memTypeIndex;
    p.size = kPageSize;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize  = kPageSize;
    mai.memoryTypeIndex = memTypeIndex;
    if (vkAllocateMemory(_device, &mai, nullptr, &p.memory) != VK_SUCCESS) {
        ENGINE_LOG_ERROR("[VK][mem] page alloc (%llu bytes, type %u) failed",
                         static_cast<unsigned long long>(kPageSize), memTypeIndex);
        return {};
    }
    if (hostVisible) {
        if (vkMapMemory(_device, p.memory, 0, kPageSize, 0, &p.mapped) != VK_SUCCESS) {
            p.mapped = nullptr;
        }
    }
    p.freeList.push_back({0, kPageSize});
    _pages.push_back(std::move(p));
    // 递归一次（新页必然 first-fit 成功）
    return allocFromPage(memTypeIndex, req, hostVisible);
}

void VkMemoryAllocator::free(const VkMemAlloc& a) {
    if (!a.valid()) return;

    if (a.pageMem == VK_NULL_HANDLE) {
        // 专用分配
        for (size_t i = 0; i < _dedicated.size(); ++i) {
            if (_dedicated[i].memory == a.memory) {
                if (_dedicated[i].mapped) {
                    vkUnmapMemory(_device, a.memory);
                }
                vkFreeMemory(_device, a.memory, nullptr);
                _dedicated.erase(_dedicated.begin() + static_cast<long>(i));
                --_liveCount;
                return;
            }
        }
        ENGINE_LOG_WARN("[VK][mem] free: unknown dedicated allocation");
        return;
    }

    // 按页身份（memory 句柄）定位——下标会因页回收 erase 而错位
    for (size_t i = 0; i < _pages.size(); ++i) {
        if (_pages[i].memory != a.pageMem) continue;
        Page& p = _pages[i];

        // 插入并按 offset 升序 + 邻接合并
        FreeBlock fb{a.offset, a.size};
        auto it = std::upper_bound(p.freeList.begin(), p.freeList.end(), fb,
                                   [](const FreeBlock& v, const FreeBlock& o) {
                                       return v.offset < o.offset;
                                   });
        it = p.freeList.insert(it, fb);
        // 与后块合并
        if (it + 1 != p.freeList.end() && it->offset + it->size == (it + 1)->offset) {
            it->size += (it + 1)->size;
            p.freeList.erase(it + 1);
        }
        // 与前块合并
        if (it != p.freeList.begin() && (it - 1)->offset + (it - 1)->size == it->offset) {
            (it - 1)->size += it->size;
            p.freeList.erase(it);
        }

        if (p.refCount > 0) --p.refCount;
        // 整页空闲 → 回收页（页身份是句柄，erase 不影响其他分配的归属）
        if (p.refCount == 0 && p.freeList.size() == 1 && p.freeList[0].size == p.size) {
            if (p.mapped) {
                vkUnmapMemory(_device, p.memory);
            }
            vkFreeMemory(_device, p.memory, nullptr);
            _pages.erase(_pages.begin() + static_cast<long>(i));
        }
        --_liveCount;
        return;
    }
    ENGINE_LOG_WARN("[VK][mem] free: page not found (already recycled?)");
}

} // namespace rhi
