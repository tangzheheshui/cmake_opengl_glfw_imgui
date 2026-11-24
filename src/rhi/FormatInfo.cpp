//
//  FormatInfo.cpp
//
#include "FormatInfo.h"
#include <glad/glad.h>
#include <cassert>

#if defined(ENGINE_HAS_VULKAN)
#include <vulkan/vulkan.h>
#define VK_FMT(f) static_cast<uint32_t>(f)
#else
#define VK_FMT(f) 0u
#endif

namespace rhi {

namespace {

// 顺序必须与 RhiTypes.h 的 Format 枚举一致（RGBA8_UNorm=0 … R32_UInt=10）。
// 每行：字节数、是否深度、是否 sRGB、是否整型、GL 内部格式、GL 数据格式、GL 数据类型、VK 格式。
const FormatInfo kTable[] = {
    /* RGBA8_UNorm */ FormatInfo(4,  false, false, false, GL_RGBA8,              GL_RGBA,           GL_UNSIGNED_BYTE,     VK_FMT(VK_FORMAT_R8G8B8A8_UNORM)),
    /* Depth32F    */ FormatInfo(4,  true,  false, false, GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT,             VK_FMT(VK_FORMAT_D32_SFLOAT)),
    /* RGBA8_SRGB  */ FormatInfo(4,  false, true,  false, GL_SRGB8_ALPHA8,       GL_RGBA,           GL_UNSIGNED_BYTE,     VK_FMT(VK_FORMAT_R8G8B8A8_SRGB)),
    /* BGRA8_UNorm */ FormatInfo(4,  false, false, false, GL_RGBA8,              GL_BGRA,           GL_UNSIGNED_BYTE,     VK_FMT(VK_FORMAT_B8G8R8A8_UNORM)),
    /* BGRA8_SRGB  */ FormatInfo(4,  false, true,  false, GL_SRGB8_ALPHA8,       GL_BGRA,           GL_UNSIGNED_BYTE,     VK_FMT(VK_FORMAT_B8G8R8A8_SRGB)),
    /* R8_UNorm    */ FormatInfo(1,  false, false, false, GL_R8,                 GL_RED,            GL_UNSIGNED_BYTE,     VK_FMT(VK_FORMAT_R8_UNORM)),
    /* R16F        */ FormatInfo(2,  false, false, false, GL_R16F,               GL_RED,            GL_FLOAT,             VK_FMT(VK_FORMAT_R16_SFLOAT)),
    /* RGBA16F     */ FormatInfo(8,  false, false, false, GL_RGBA16F,            GL_RGBA,           GL_FLOAT,             VK_FMT(VK_FORMAT_R16G16B16A16_SFLOAT)),
    /* RGBA32F     */ FormatInfo(16, false, false, false, GL_RGBA32F,            GL_RGBA,           GL_FLOAT,             VK_FMT(VK_FORMAT_R32G32B32A32_SFLOAT)),
    /* D24S8       */ FormatInfo(4,  true,  false, false, GL_DEPTH24_STENCIL8,   GL_DEPTH_STENCIL,  GL_UNSIGNED_INT_24_8, VK_FMT(VK_FORMAT_D24_UNORM_S8_UINT)),
    /* R32_UInt    */ FormatInfo(4,  false, false, true,  GL_R32UI,              GL_RED_INTEGER,    GL_UNSIGNED_INT,      VK_FMT(VK_FORMAT_R32_UINT)),
};

} // namespace

bool FormatInfo::fromVkFormat(uint32_t vkFormat, Format& out) {
    if (vkFormat == 0) return false;
    // 枚举值从 0 连续排列（RhiTypes.h 只允许追加），逐个比对即可
    for (int i = 0; i <= static_cast<int>(Format::R32_UInt); ++i) {
        const Format f = static_cast<Format>(i);
        if (of(f).vkFormat() == vkFormat) {
            out = f;
            return true;
        }
    }
    return false;
}

const FormatInfo& FormatInfo::of(Format f) {
    const int i = static_cast<int>(f);
    const int n = static_cast<int>(sizeof(kTable) / sizeof(kTable[0]));
    assert(i >= 0 && i < n && "Format 越界：FormatInfo 表与 RhiTypes.h 的 Format 枚举不同步");
    return kTable[(i >= 0 && i < n) ? i : 0];
}

} // namespace rhi
