//
//  Sky.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/4/7.
//

#include "Sky.h"
#include "render/camera/camera.h"
#include "render/RenderSystem.h"
#include "rhi/IRHIDevice.h"
#include "rhi/IRHICommandList.h"
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
#include <vector>
#include "fs/FilePathUtil.h"
#include "stb_image.h"
#include "log/LogMacros.h"

// 立方体顶点（36 顶点，仅位置）
static const float kSkyVertices[] = {
    -1.0f,  1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,   1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,  -1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,   1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,   1.0f, -1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f, -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
    -1.0f, -1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f, -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
    -1.0f,  1.0f, -1.0f,   1.0f,  1.0f, -1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f,
};

bool Sky::draw(const Matrix &mtx) {
    return false;
}

void Sky::setCubeImage(const std::array<std::string, 6> &imgs) {
    _arrImage = imgs;
}


// ================= M4.3: RHI 路径 =================
void Sky::ensureRHIResources(rhi::IRHIDevice* device) {
    if (m_rhiBuilt) return;

    rhi::GraphicsPipelineDesc desc;
    desc.shaders = {
        { rhi::ShaderStage::Vertex,   "vert_sky" },
        { rhi::ShaderStage::Fragment, "frag_sky" },
    };
    desc.vertexLayout.stride = 3 * sizeof(float);
    desc.vertexLayout.attributes = {
        { 0, rhi::VertexFormat::Float3, 0 },
    };
    desc.primitive = rhi::PrimitiveType::Triangles;
    desc.cull      = rhi::CullMode::None;
    // 天空盒：深度测试开 + LEQUAL + 不写深度（远景绘制标准做法）
    desc.depth     = { true, false, true };
    desc.blend     = { false };
    m_rhiPipeline = device->createPipeline(desc);

    rhi::BufferDesc vbDesc;
    vbDesc.size = sizeof(kSkyVertices);
    vbDesc.usage = rhi::BUF_VERTEX;
    vbDesc.initialData = kSkyVertices;
    m_rhiVB = device->createBuffer(vbDesc);

    // 立方体贴图：加载 6 面 → RGBA 打包 → RHI 纹理（不做纵向翻转，cubemap 面序与 stb 一致）
    const size_t faceBytes = static_cast<size_t>(512) * 0;   // 尺寸以实际加载为准
    (void)faceBytes;
    std::vector<unsigned char> packed;
    int fw = 0, fh = 0, fch = 0;
    bool ok = true;
    for (int face = 0; face < 6 && ok; ++face) {
        int w = 0, h = 0, ch = 0;
        // A4: UTF-8 路径内存解码
        std::vector<unsigned char> fileData;
        if (!engine::fs::readFileBinary(engine::fs::pathFromUtf8(_arrImage[face]), fileData)) {
            ENGINE_LOG_ERROR("[SKY] cubemap face open failed: %s", _arrImage[face].c_str());
            ok = false;
            break;
        }
        unsigned char* d = stbi_load_from_memory(fileData.data(),
                                                 static_cast<int>(fileData.size()),
                                                 &w, &h, &ch, 0);
        if (!d || w <= 0 || h <= 0) { ok = false; break; }
        if (fw == 0) { fw = w; fh = h; fch = ch; packed.reserve(static_cast<size_t>(fw) * fh * 4 * 6); }
        else if (w != fw || h != fh) { stbi_image_free(d); ok = false; break; }
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const size_t o = (static_cast<size_t>(y) * w + x) * ch;
                packed.push_back(d[o + 0]);
                packed.push_back(d[o + 1]);
                packed.push_back(d[o + 2]);
                packed.push_back(ch == 4 ? d[o + 3] : 255);
            }
        }
        stbi_image_free(d);
    }
    if (!ok) {
        ENGINE_LOG_ERROR("[SKY] cubemap load failed");
        return;
    }

    rhi::TextureDesc td;
    td.width = static_cast<uint32_t>(fw);
    td.height = static_cast<uint32_t>(fh);
    td.usage = rhi::TEX_SAMPLED;
    // B3c：天空盒是颜色贴图；sRGB 解码 + sRGB 交换链编码 = 往返不变
    td.format = rhi::Format::RGBA8_SRGB;
    td.cubemap = true;
    td.initialData = packed.data();
    // B3：天空盒远景明显 — 开 mipmap（GPU blit 生成链）+ 各向异性过滤，
    // 消除远处地平线闪烁；cubemap 地址模式由后端强制 ClampToEdge
    td.generateMips = true;
    td.sampler.maxAnisotropy = 8.0f;
    m_rhiCubeTex = device->createTexture(td);
    m_rhiBuilt = true;
}

bool Sky::drawRHI(rhi::IRHICommandList* cmd, const Matrix &viewMat, const Matrix &projMat) {
    auto* device = RenderSystem::getInstance().rhiDevice();
    if (!device) return false;
    ensureRHIResources(device);
    if (!m_rhiPipeline.valid() || !m_rhiVB.valid() || !m_rhiCubeTex.valid()) return false;

    cmd->bindPipeline(m_rhiPipeline);
    glm::mat4 v = Matrix::toMatrix(viewMat);
    glm::mat4 p = Matrix::toMatrix(projMat);
    cmd->setUniform("uViewMat", rhi::UniformValue::fromMat4(glm::value_ptr(v)));
    cmd->setUniform("uProjMat", rhi::UniformValue::fromMat4(glm::value_ptr(p)));
    cmd->bindTexture(m_rhiCubeTex, 3, "uTexture");   // 槽 3 = cubemap（VK binding4）
    cmd->bindVertexBuffer(m_rhiVB);
    cmd->draw(36);
    return true;
}
