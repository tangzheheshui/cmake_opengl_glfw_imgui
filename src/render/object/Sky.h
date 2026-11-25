//
//  Sky.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/4/7.
//

#ifndef Sky_hpp
#define Sky_hpp

#include <string>
#include <array>
#include "../BaseDraw.h"
#include "rhi/RhiTypes.h"

namespace rhi { class IRHICommandList; }

class Sky : public BaseDraw {
public:
    Sky() {}
    ~Sky() {}
    virtual bool draw(const Matrix &mtx) override;
    // M4.3: RHI 路径（在活跃 render pass 内录制；两后端通用）
    bool drawRHI(rhi::IRHICommandList* cmd, const Matrix &viewMat, const Matrix &projMat);
    void setCubeImage(const std::array<std::string, 6>& imgs);
private:
    void ensureRHIResources(class rhi::IRHIDevice* device);
    std::array<std::string, 6> _arrImage;

    rhi::BufferHandle m_rhiVB;
    rhi::PipelineHandle m_rhiPipeline;
    rhi::TextureHandle m_rhiCubeTex;
    bool m_rhiBuilt = false;
};

#endif /* Sky_hpp */
