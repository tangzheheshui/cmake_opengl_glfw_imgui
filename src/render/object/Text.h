//
//  Text.hpp
//  OpenglLearn
//
#ifndef Text_hpp
#define Text_hpp

#include <iostream>
#include <string>

#include "../BaseDraw.h"
#include "rhi/RhiTypes.h"

class FontManager;
namespace rhi { class IRHIDevice; }

class CText : public BaseDraw {
public:
    CText(std::weak_ptr<FontManager> fontMng);
    ~CText() {}
    virtual bool draw(const Matrix &mtx) override;
    bool isOverlayDraw() const override { return true; }   // D4：天空之后绘制
    void SetText(const std::string &str);
private:
    // ---- D4: SDF 文字 RHI 路径（GL/VK 同一实现；图集 + 动态顶点 + blend 管线）----
    bool drawRHI(const Matrix &mtx);
    void ensureRHIResources(rhi::IRHIDevice* device);
private:
    std::weak_ptr<FontManager> _fontMng;
    std::string m_str;

    rhi::PipelineHandle m_rhiPipeline;
    rhi::BufferHandle m_rhiVB;
    rhi::BufferHandle m_rhiIB;
    rhi::TextureHandle m_rhiAtlas;
    rhi::UniformBlockHandle m_rhiUB;   // VK 描述符布局固定，bindTexture 前需专属块
    std::vector<float> m_rhiVerts;     // 每字符 4 顶点（pos3+uv2 = 20B）
    std::string m_rhiBuiltStr;         // 字符串未变不重灌 VB
    bool m_rhiBuilt = false;
};

#endif /* Text_hpp */
