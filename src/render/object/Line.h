//
//  Line.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/25.
//

#ifndef Line_hpp
#define Line_hpp

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
    void setWidth(float width) { m_fWidth = width; }
    virtual bool draw(const Matrix &mtx) override;
private:
    void calculate();
    // ---- M2: RHI 迁移路径（与原 GL 路径并存，调试面板开关切换）----
    bool drawRHI(const Matrix &mtx);
    void ensureRHIResources(rhi::IRHIDevice* device);
private:
    std::vector<glm::vec3> m_points;
    std::vector<unsigned int> m_indices;
    glm::vec3 m_color;
    float m_fWidth = 1;

    rhi::PipelineHandle m_rhiPipeline;
    rhi::BufferHandle m_rhiVB;
    rhi::BufferHandle m_rhiIB;
    bool m_rhiDirty = true;   // 数据变化未上传标记
};

#endif /* Line_hpp */
