//
//  Sky.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/4/7.
//

#include "Sky.h"
#include "render/shader.h"
#include "render/camera/camera.h"
#include "render/image.h"
#include "render/RenderSystem.h"
#include "rhi/IRHIDevice.h"
#include "rhi/IRHICommandList.h"
#include <glm/gtc/type_ptr.hpp>
#include <cstring>
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

bool Sky::draw(const Matrix &viewMat, const Matrix &projMat) {
    float skyboxVertices[] = {
        // positions          
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
        1.0f, -1.0f, -1.0f,
        1.0f, -1.0f, -1.0f,
        1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        
        -1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,
        
        1.0f, -1.0f, -1.0f,
        1.0f, -1.0f,  1.0f,
        1.0f,  1.0f,  1.0f,
        1.0f,  1.0f,  1.0f,
        1.0f,  1.0f, -1.0f,
        1.0f, -1.0f, -1.0f,
        
        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        1.0f,  1.0f,  1.0f,
        1.0f,  1.0f,  1.0f,
        1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,
        
        -1.0f,  1.0f, -1.0f,
        1.0f,  1.0f, -1.0f,
        1.0f,  1.0f,  1.0f,
        1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,
        
        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
        1.0f, -1.0f, -1.0f,
        1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
        1.0f, -1.0f,  1.0f
    };
    
    if (!_has_bind_vbo) {
        glBindVertexArray(_VAO);
        glBindBuffer(GL_ARRAY_BUFFER, _VBO);
        glBufferData(GL_ARRAY_BUFFER, sizeof(float) * 18 * 6, skyboxVertices, GL_STATIC_DRAW);
        // position attribute
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        _has_bind_vbo = true;
    }
    
    auto shader = ShaderCache::GetInstance().GetShader(ShaderType::Sky);
    if (!shader) {
        return false;
    }
    shader->use();
    
    if (_textureID == 0) {
        _textureID = TextureMng::getInstance().getCubeTexture(_arrImage);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, _textureID);
    shader->setInt("uTexture", 0);
    
    // 矩阵
    shader->setMat4("uViewMat", viewMat);
    shader->setMat4("uProjMat", projMat);
    
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glBindVertexArray(_VAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    return true;
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
        unsigned char* d = stbi_load(_arrImage[face].c_str(), &w, &h, &ch, 0);
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
    cmd->bindTexture(m_rhiCubeTex, 3);   // 槽 3 = cubemap（VK binding4）
    cmd->bindVertexBuffer(m_rhiVB);
    cmd->draw(36);
    return true;
}
