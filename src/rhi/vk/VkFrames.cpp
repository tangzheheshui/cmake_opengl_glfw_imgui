//
//  VkFrames.cpp
//
#include "VkFrames.h"
#include <cstdio>

namespace rhi {

bool VkFrames::init(VkDevice device, VkCommandPool pool, uint32_t slots, NameFn nameFn) {
    _device = device;
    if (!nameFn) nameFn = [](VkObjectType, uint64_t, const char*) {};
    if (slots < 1) slots = 1;

    _slots.resize(slots);
    for (uint32_t i = 0; i < slots; ++i) {
        Slot& s = _slots[i];

        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool        = pool;
        ai.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(device, &ai, &s.cmd) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] frame %u: command buffer alloc failed", i);
            return false;
        }

        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;   // 首帧可直接等待
        if (vkCreateFence(device, &fci, nullptr, &s.fence) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] frame %u: fence create failed", i);
            return false;
        }

        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(device, &sci, nullptr, &s.imageAvailable) != VK_SUCCESS) {
            ENGINE_LOG_ERROR("[VK] frame %u: image-available semaphore create failed", i);
            return false;
        }

        char name[64];
        std::snprintf(name, sizeof(name), "RHI_Frame%u_CmdBuffer", i);
        nameFn(VK_OBJECT_TYPE_COMMAND_BUFFER, reinterpret_cast<uint64_t>(s.cmd), name);
        std::snprintf(name, sizeof(name), "RHI_Frame%u_InFlightFence", i);
        nameFn(VK_OBJECT_TYPE_FENCE, reinterpret_cast<uint64_t>(s.fence), name);
        std::snprintf(name, sizeof(name), "RHI_Frame%u_ImageAvailable", i);
        nameFn(VK_OBJECT_TYPE_SEMAPHORE, reinterpret_cast<uint64_t>(s.imageAvailable), name);
    }
    _index = 0;
    return true;
}

void VkFrames::shutdown() {
    for (Slot& s : _slots) {
        if (s.fence)          vkDestroyFence(_device, s.fence, nullptr);
        if (s.imageAvailable) vkDestroySemaphore(_device, s.imageAvailable, nullptr);
        s.fence = VK_NULL_HANDLE;
        s.imageAvailable = VK_NULL_HANDLE;
        s.cmd = VK_NULL_HANDLE;   // 命令缓冲随命令池销毁
    }
    _slots.clear();
}

bool VkFrames::wait() {
    VkResult r = vkWaitForFences(_device, 1, &current().fence, VK_TRUE, UINT64_MAX);
    return r == VK_SUCCESS;
}

bool VkFrames::beginRecording() {
    Slot& s = current();
    if (vkResetFences(_device, 1, &s.fence) != VK_SUCCESS) return false;
    if (vkResetCommandBuffer(s.cmd, 0) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    return vkBeginCommandBuffer(s.cmd, &bi) == VK_SUCCESS;
}

} // namespace rhi
