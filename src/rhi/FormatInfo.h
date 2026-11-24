//
//  FormatInfo.h
//  RHI 抽象层 - 像素格式唯一真相源
//
//  所有后端共享的格式知识（字节数、深度/sRGB/整型标志、GL 格式三元组、VK 格式）
//  集中在这一张表里。新增格式只改 FormatInfo.cpp 一处，避免 RhiTypes/GL/VK 三处 switch 漂移。
//
#ifndef FormatInfo_h
#define FormatInfo_h

#include "RhiTypes.h"
#include <cstdint>

namespace rhi {

class FormatInfo {
public:
    // 按 Format 取格式信息；越界触发断言（表与 Format 枚举不同步时暴露）。
    static const FormatInfo& of(Format f);

    uint32_t bytesPerPixel()    const { return _bytes; }
    bool     isDepth()          const { return _depth; }
    bool     isSRGB()           const { return _srgb; }
    bool     isInteger()        const { return _integer; }
    uint32_t glInternalFormat() const { return _glInternal; }   // GLenum
    uint32_t glDataFormat()     const { return _glDataFmt; }    // GLenum
    uint32_t glDataType()       const { return _glDataType; }   // GLenum
    uint32_t vkFormat()         const { return _vk; }           // VkFormat；GL-only 构建恒 0

    FormatInfo(uint32_t bytes, bool depth, bool srgb, bool integer,
               uint32_t glInternal, uint32_t glDataFmt, uint32_t glDataType, uint32_t vk)
        : _bytes(bytes), _depth(depth), _srgb(srgb), _integer(integer),
          _glInternal(glInternal), _glDataFmt(glDataFmt), _glDataType(glDataType), _vk(vk) {}

private:
    uint32_t _bytes = 0;
    bool     _depth = false;
    bool     _srgb  = false;
    bool     _integer = false;
    uint32_t _glInternal = 0;
    uint32_t _glDataFmt  = 0;
    uint32_t _glDataType = 0;
    uint32_t _vk = 0;
};

} // namespace rhi

#endif /* FormatInfo_h */
