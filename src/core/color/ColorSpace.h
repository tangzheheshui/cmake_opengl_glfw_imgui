//
//  ColorSpace.h
//  sRGB ↔ linear 转换（IEC 61966-2-1）。
//
//  B3c 起引擎采用线性工作流：
//    - 颜色纹理用 sRGB 格式（硬件采样时解码到 linear）
//    - 光照/混合在线性空间进行
//    - 交换链/默认帧缓冲用 sRGB 格式（硬件写回时编码）
//  因此“作者指定为 sRGB”的常量（清屏色、顶点色、ImGui 样式色）在上传前
//  必须经 srgbToLinear；纹理与帧缓冲的转换由硬件完成。
//
#ifndef ColorSpace_h
#define ColorSpace_h

#include <algorithm>
#include <cmath>

namespace color {

inline float srgbToLinear(float c) {
    if (c <= 0.04045f) return c / 12.92f;
    return std::pow((c + 0.055f) / 1.055f, 2.4f);
}

inline float linearToSrgb(float c) {
    if (c <= 0.0031308f) return c * 12.92f;
    return 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
}

// 原地转换 3/4 分量颜色（alpha 不参与）
inline void srgbToLinear(float* rgba, int components) {
    if (!rgba) return;
    const int n = std::min(components, 3);
    for (int i = 0; i < n; ++i) rgba[i] = srgbToLinear(rgba[i]);
}

} // namespace color

#endif /* ColorSpace_h */
