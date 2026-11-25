//
//  Line.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/25.
//

#ifndef Line_hpp
#define Line_hpp

#include <cstdint>
#include <glm/glm.hpp>
#include <vector>
#include "../BaseDraw.h"
#include "rhi/RhiTypes.h"

namespace rhi { class IRHIDevice; }

class Line : public BaseDraw {
public:
    Line() {}
    void setData(const std::vector<glm::vec3> &points, const std::vector<unsigned int> &indices);
    void setColor(const glm::vec3 &color) { m_color = color;}
    // 线宽（屏幕像素，默认 1）。RHI 路径（GL/VK 同一实现）：
    // 屏幕空间三角形展开，宽度像素级精确。
    void setWidth(float width) { m_fWidth = width; m_rhiDirty = true; }
    // 视口尺寸提示（framebuffer 像素）。仅在命令列表拿不到视口时兜底；
    // 默认 3200x1800 = 本 app 默认窗口（1600x900）在 macOS Retina 2x 下的尺寸。
    void setViewportSizeHint(uint32_t w, uint32_t h) { m_rhiViewportHintW = w; m_rhiViewportHintH = h; }
    virtual bool draw(const Matrix &mtx) override;
private:
    bool drawRHI(const Matrix &mtx);
    void ensureRHIResources(rhi::IRHIDevice* device);
    // ---- D1: 宽线展开路径 ----
    // 展开顶点：线段端点 A/B + 展开参数 + 像素→NDC 换算，共 12 float = 48B
    struct ExpandVertex {
        glm::vec3 posA;
        glm::vec3 posB;
        glm::vec4 meta;    // (端别 0=A/1=B, 侧别 ±1, 宽度像素, 保留)
        glm::vec2 screen;  // (2/viewportW, 2/viewportH)
    };
    void refreshRhiViewport();                    // 问命令列表当前视口像素尺寸
    void rebuildRhiExpandData();                  // 线段 → 三角形顶点/索引（CPU 展开）
    void uploadRhiExpandData(rhi::IRHIDevice* device);
private:
    std::vector<glm::vec3> m_points;
    std::vector<unsigned int> m_indices;
    glm::vec3 m_color;
    float m_fWidth = 1;

    rhi::PipelineHandle m_rhiPipeline;
    rhi::BufferHandle m_rhiVB;
    rhi::BufferHandle m_rhiIB;
    bool m_rhiDirty = true;   // 数据/宽度变化未上传标记

    std::vector<ExpandVertex> m_rhiVerts;         // 展开后的三角形顶点
    std::vector<unsigned int> m_rhiIndices;       // 展开后的三角形索引
    uint32_t m_rhiViewportW = 0;                  // 当前视口（像素）
    uint32_t m_rhiViewportH = 0;
    uint32_t m_rhiBakedViewportW = 0;             // 最近一次烘焙所用视口（变化时才重烘）
    uint32_t m_rhiBakedViewportH = 0;
    uint32_t m_rhiViewportHintW = 3200;           // VK 用视口尺寸提示（GL 走真实查询）
    uint32_t m_rhiViewportHintH = 1800;
};

#endif /* Line_hpp */
