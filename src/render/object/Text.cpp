

#include "Text.h"
#include "FontManager.h"
#include "RenderSystem.h"
#include "rhi/IRHICommandList.h"
#include "log/LogMacros.h"
#include "color/ColorSpace.h"

#include <cstring>

namespace {
constexpr uint32_t kMaxTextChars = 1024;   // 动态顶点容量（字符数）
constexpr float    kTextScale = 0.5f;      // 与 GL 遗留路径一致的世界缩放
}

CText::CText(std::weak_ptr<FontManager> fontMng) {
    _fontMng = fontMng;
    // 去后端化：几何/纹理/管线全部在 RHI 路径惰性创建（构造函数禁止 GL 调用）
}

// ---- D4: SDF 文字 RHI 路径 ----
// 图集一次构建（FontManager CPU 侧），纹理/管线/VB 惰性建；每字符串变化时
// 重灌顶点（quad 展开，几何约定与 GL 遗留路径一致：字形顶 = v0 行）。
void CText::ensureRHIResources(rhi::IRHIDevice* device) {
    if (m_rhiBuilt) return;
    auto font = _fontMng.lock();
    if (!font || !font->getSdfGlyph('A')) {   // 图集惰性构建 + 可用性检查
        return;
    }

    // 图集纹理：R8 单通道、线性过滤、ClampToEdge（无 mip）
    rhi::TextureDesc td;
    td.width  = font->sdfAtlasWidth();
    td.height = font->sdfAtlasHeight();
    td.format = rhi::Format::R8_UNorm;
    td.usage  = rhi::TEX_SAMPLED;
    td.sampler.minFilter = rhi::FilterMode::Linear;
    td.sampler.magFilter = rhi::FilterMode::Linear;
    td.sampler.mipFilter = rhi::FilterMode::Linear;
    td.sampler.wrapU = td.sampler.wrapV = td.sampler.wrapW = rhi::WrapMode::ClampToEdge;
    td.initialData = font->sdfAtlasData().data();
    m_rhiAtlas = device->createTexture(td);
    if (!m_rhiAtlas.valid()) return;

    // 管线：blend + 无深度（overlay 语义；PC 布局 mat4@0 + vec4 uColor@64）
    rhi::GraphicsPipelineDesc pd;
    pd.shaders = {
        { rhi::ShaderStage::Vertex,   "vert_text_rhi" },
        { rhi::ShaderStage::Fragment, "frag_text_rhi" },
    };
    struct TextVertex { float pos[3]; float uv[2]; };
    pd.vertexLayout.stride = sizeof(TextVertex);
    pd.vertexLayout.attributes = {
        { 0, rhi::VertexFormat::Float3, 0 },
        { 1, rhi::VertexFormat::Float2, 12 },
    };
    pd.primitive = rhi::PrimitiveType::Triangles;
    pd.cull      = rhi::CullMode::None;
    pd.depth     = { false, false };
    pd.blend     = { true };
    m_rhiPipeline = device->createPipeline(pd);

    // 动态顶点缓冲 + 静态索引（每字符 6 索引，一次建满容量）
    rhi::BufferDesc vbDesc;
    vbDesc.size = kMaxTextChars * 4 * 5 * sizeof(float);
    vbDesc.usage = rhi::BUF_CPU_WRITE;
    m_rhiVB = device->createBuffer(vbDesc);

    std::vector<unsigned int> indices(kMaxTextChars * 6);
    for (uint32_t i = 0; i < kMaxTextChars; ++i) {
        const unsigned int b = i * 4;
        indices[i * 6 + 0] = b + 0;
        indices[i * 6 + 1] = b + 1;
        indices[i * 6 + 2] = b + 2;
        indices[i * 6 + 3] = b + 0;
        indices[i * 6 + 4] = b + 3;
        indices[i * 6 + 5] = b + 1;
    }
    rhi::BufferDesc ibDesc;
    ibDesc.size = static_cast<uint32_t>(indices.size() * sizeof(unsigned int));
    ibDesc.usage = rhi::BUF_INDEX;
    ibDesc.initialData = indices.data();
    m_rhiIB = device->createBuffer(ibDesc);

    // VK 描述符集专属块（bindTexture 写"当前块"，避免污染模型纹理槽）
    m_rhiUB = device->createUniformBlock(256);

    m_rhiVerts.reserve(kMaxTextChars * 4 * 5);
    m_rhiBuilt = m_rhiPipeline.valid() && m_rhiVB.valid() && m_rhiIB.valid();
    if (m_rhiBuilt) {
        ENGINE_LOG_INFO("[TEXT] sdf resources ready (atlas %ux%u)",
                        font->sdfAtlasWidth(), font->sdfAtlasHeight());
    }
}

bool CText::drawRHI(rhi::IRHICommandList* cmd, const Matrix &mtx) {
    auto& rs = RenderSystem::getInstance();
    auto* device = rs.rhiDevice();
    auto font = _fontMng.lock();
    if (!device || !font) return false;

    ensureRHIResources(device);
    if (!m_rhiBuilt) return false;

    // 字符串变化才重建顶点（世界字形展开与 GL 遗留路径逐字段一致）
    if (m_rhiBuiltStr != m_str) {
        m_rhiBuiltStr = m_str;
        m_rhiVerts.clear();
        float x = 0, y = 0;
        const float z = 0.5f;
        const uint32_t aw = font->sdfAtlasWidth();
        const uint32_t ah = font->sdfAtlasHeight();
        for (unsigned char c : m_str) {
            const SdfGlyph* g = font->getSdfGlyph(c);
            if (!g) continue;
            const float xpos = x + g->bearingX * kTextScale;
            const float ypos = y - (g->height - g->bearingY) * kTextScale;
            const float w = g->width * kTextScale;
            const float h = g->height * kTextScale;
            const float u0 = static_cast<float>(g->u0) / aw, v0 = static_cast<float>(g->v0) / ah;
            const float u1 = static_cast<float>(g->u1) / aw, v1 = static_cast<float>(g->v1) / ah;
            // 与遗留路径同序：0=左上 1=右下 2=左下（三角形 0,1,2 / 0,3,1）
            const float quad[4][5] = {
                { xpos,     ypos + h, z, u0, v0 },
                { xpos + w, ypos,     z, u1, v1 },
                { xpos,     ypos,     z, u0, v1 },
                { xpos + w, ypos + h, z, u1, v0 },
            };
            m_rhiVerts.insert(m_rhiVerts.end(), quad[0], quad[0] + 20);
            x += static_cast<float>(g->advance) * kTextScale;
        }
        if (!m_rhiVerts.empty()) {
            device->updateBuffer(m_rhiVB, m_rhiVerts.data(),
                                 static_cast<uint32_t>(m_rhiVerts.size() * sizeof(float)));
        }
    }
    if (m_rhiVerts.empty()) return true;   // 空串：无事可画

    // cmd 由调用方传入（多线程录制）
    if (!cmd) return false;

    // B3c：作者 sRGB 常量上传前转线性
    float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
    color::srgbToLinear(color, 3);

    cmd->bindPipeline(m_rhiPipeline);
    cmd->setUniform("uMatrixMVP", rhi::UniformValue::fromMat4(mtx.buffer()));
    cmd->setUniform("uColor", rhi::UniformValue::fromVec4(color));
    // VK：切专属块再绑图集（同贴图四边形的隔离约定）；GL 按名直传不受影响
    static const unsigned char zeroBlock[256] = {};
    cmd->setDrawUniforms(m_rhiUB, zeroBlock, sizeof(zeroBlock));
    cmd->bindTexture(m_rhiAtlas, 0, "uAtlas");
    cmd->bindVertexBuffer(m_rhiVB);
    cmd->bindIndexBuffer(m_rhiIB);
    const uint32_t chars = static_cast<uint32_t>(m_str.size());
    cmd->drawIndexed(chars * 6);
    return true;
}

bool CText::draw(rhi::IRHICommandList* cmd, const Matrix &mtx) {
    // 两端同一实现：SDF 图集 RHI 路径
    return drawRHI(cmd, mtx);
}

void CText::SetText(const std::string &str) {
    m_str = str;
}
