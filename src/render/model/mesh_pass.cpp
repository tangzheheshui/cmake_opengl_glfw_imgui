//
//  pass.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/22.
//

#include "mesh_pass.h"
#include "RenderSystem.h"
#include "rhi/IRHIDevice.h"
#include "rhi/IRHICommandList.h"
#include "render/DrawUniforms.h"
#include "color/ColorSpace.h"
#include "Light.h"
#include "../scene.h"
#include "../Light.h"
#include "image.h"

MeshPass::MeshPass(std::shared_ptr<MeshData> meshData, std::shared_ptr<Material> matData) {
    m_mesh_data = meshData;
    m_material = matData;
}

bool MeshPass::DrawPick(const std::vector<Matrix> &matModel, const Matrix &mtx, uint16_t pickID) {
    auto shader = ShaderCache::GetInstance().GetShader(ShaderType::Pick);   
    if (!shader) {
        return false;
    }
    
    shader->use();
    
    shader->setInt("uPickID", pickID);
    
    setup(matModel, 0);
  
    // 绘制网格
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glBindVertexArray(_VAO);
    glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)m_mesh_data->indices.size(), GL_UNSIGNED_INT, 0,  (int)matModel.size());
    glBindVertexArray(0);
    
    return true;
}

// D5：线框颜色（深灰蓝，CAD 常见边线观感；线性空间直出）
static glm::vec4 wireColorOf() {
    return glm::vec4(0.13f, 0.14f, 0.18f, 1.0f);
}

// 悬停提示值（淡白蓝；选中对象不叠加悬停色）
static glm::vec4 hoverValueOf(uint32_t flags) {
    if (!(flags & DrawOption::DRAW_HOVER) || (flags & DrawOption::DRAW_HIGHLIGHT)) {
        return glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float strength = RenderSystem::getInstance().settings().hoverStrength;
    return glm::vec4(0.60f, 0.75f, 0.95f, strength);
}

// D8：高亮 push/uniform 值（选中 = 淡蓝 × 强度；未选中 = 全零即关闭）
static glm::vec4 highlightValueOf(uint32_t flags) {
    if (!(flags & DrawOption::DRAW_HIGHLIGHT)) {
        return glm::vec4(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const float strength = RenderSystem::getInstance().settings().highlightStrength;
    return glm::vec4(0.25f, 0.55f, 1.0f, strength);
}

bool MeshPass::Draw(const std::vector<Matrix> &matModel, const Matrix &mtx, uint32_t flags, int numViewpoit, float opacity) {
    // M4.2: RHI 路由（Vulkan 后端必走；GL 后端可经 modelUseRHI 开关切换对比）
    {
        auto& rs = RenderSystem::getInstance();
        auto* device = rs.rhiDevice();
        bool useRHI = device && (device->backend() == rhi::Backend::Vulkan
                                 || rs.settings().modelUseRHI);
        if (useRHI) {
            return DrawRHI(matModel, mtx, flags, opacity);
        }
    }

    bool bDrawShadow = (flags & DrawOption::DRAW_SHADOW);
    auto shader = getShader(flags);
    if (!shader) {
        return false;
    }
    
    shader->use();

    // D7：剖切平面（关闭时传 (0,0,0,1)，dot + 1 >= 0 不裁剪任何点）
    {
        auto& settings = RenderSystem::getInstance().settings();
        glm::vec4 clipPlane = settings.clipPlaneEnabled
            ? glm::vec4(settings.clipPlaneNormal[0], settings.clipPlaneNormal[1],
                        settings.clipPlaneNormal[2], settings.clipPlaneDistance)
            : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        shader->setFloat4("uClipPlane", clipPlane.x, clipPlane.y, clipPlane.z, clipPlane.w);
    }

    // D8：选中高亮（未选中传全零，mix 无效果）
    {
        const glm::vec4 hl = highlightValueOf(flags);
        shader->setFloat4("uHighlight", hl.x, hl.y, hl.z, hl.w);
    }
    {
        const glm::vec4 hv = hoverValueOf(flags);
        shader->setFloat4("uHover", hv.x, hv.y, hv.z, hv.w);
    }

    // D3：不透明度（GL 遗留路径的 blend 开关在 Model::draw，避免逐网格切状态）
    shader->setFloat("uOpacity", opacity);

    bool bDrawPick = (flags & DrawOption::DRAW_PICK);
 
    if (bDrawShadow) {
        auto lightSpaceMatrix = Scene::GetLightVPMatrix();
        shader->setMat4("lightSpaceMatrix", lightSpaceMatrix);
    }
    else if(bDrawPick) {
        // 设置实体的ID
        shader->setInt("uPickID", 0);
    }
    else {
        bool has_normal_tex = false;
        bool has_specular_tex = false;
        
        for (int i = 0; i < m_material->textures.size(); i++) {
            auto tex = m_material->textures[i];
            int texID = 0;
            if (!tex.filepath.empty()) {
                // B3c：diffuse/specular 是颜色贴图（sRGB 解码）；normal/height 是
                // 线性数据贴图，必须保持非 sRGB 原值。
                const bool colorTex = tex.name.rfind("texture_diffuse", 0) == 0
                                   || tex.name.rfind("texture_specular", 0) == 0;
                texID = TextureMng::getInstance().getTexture(tex.filepath, colorTex);
                if(tex.name == "texture_diffuse0") {
                    
                }
                else if(tex.name == "texture_specular0") {
                    has_specular_tex = true;
                }
                else if(tex.name == "texture_normal0") {
                    if (!m_mesh_data->tangents.empty()) {
                        has_normal_tex = true;
                    }
                }
                else if(tex.name == "texture_height0") {
                }
            }
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, texID);
            shader->setInt(tex.name, i);
            shader->setBool("uMaterail.has_normal_tex", has_normal_tex);
            shader->setBool("uMaterail.has_specular_tex", has_specular_tex);
        }
        
        // 矩阵
        shader->setMat4("uViewProj", mtx);
        
        if (numViewpoit > 1) {
            shader->setMat4Array("uMatrixModel", matModel);
            shader->setInt("uNumViewport", numViewpoit);
        }
        // 灯光
        bool lightOpen = flags&DrawOption::LIGHT_OPEN;
        shader->setBool("uLight.is_open", lightOpen);
        auto light = Light::GlobalLight();
        shader->setFloat4("uLight.position", light.position.x, light.position.y, light.position.z, 1);
        shader->setFloat3("uLight.direction", light.direction.x, light.direction.y, light.direction.z);
        shader->setFloat("uLight.cosTheta", light.cosTheta);
        shader->setFloat3("uLight.ambient", light.ambient.x, light.ambient.y, light.ambient.z);
        shader->setFloat3("uLight.diffuse", light.diffuse.x, light.diffuse.y, light.diffuse.z);
        shader->setFloat3("uLight.specular", light.specular.x, light.specular.y, light.specular.z);
        
        // 材质
        shader->setFloat3("uMaterail.ambient", m_material->ambient.r, m_material->ambient.g, m_material->ambient.b);
        shader->setFloat3("uMaterail.diffuse", m_material->diffuse.r, m_material->diffuse.g, m_material->diffuse.b);
        shader->setFloat3("uMaterail.specular", m_material->specular.r, m_material->specular.g, m_material->specular.b);
        shader->setFloat("uMaterail.shininess", m_material->shininess);
        shader->setFloat("uMaterail.shininess_strength", m_material->shininess_strength);
        
        // 相机位置
        auto cam_pos = Camera::GetCamera().getPossition();
        shader->setFloat3("uCameraPos", cam_pos.x, cam_pos.y, cam_pos.z);
        
        // boneMat
        if (m_matBone && !m_matBone->empty()) {
            for (int i = 0; i < m_matBone->size(); ++i) {
                auto mat = Matrix::toMatrix(m_matBone->at(i));
                shader->setMat4("uFinalBonesMatrices[" + std::to_string(i) + "]", mat);
            }
        }
    }
    
    setup(matModel, flags);

    // 绘制网格
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glBindVertexArray(_VAO);

    // D5：显示模式（GL 遗留路径用 glPolygonMode；拾取/阴影 Pass 不受影响）
    const int displayMode = RenderSystem::getInstance().settings().displayMode;
    const bool wireOnly = (displayMode == 1) && !bDrawShadow && !bDrawPick;
    if (wireOnly) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    }
    glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)m_mesh_data->indices.size(), GL_UNSIGNED_INT, 0,  (int)matModel.size());
    if (wireOnly) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    }
    else if ((displayMode == 2) && !bDrawShadow && !bDrawPick) {
        // 着色+边线：第二遍线框（GL_POLYGON_OFFSET_LINE 深度偏移防 z-fighting）
        glEnable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(2.0f, 2.0f);
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glDrawElementsInstanced(GL_TRIANGLES, (GLsizei)m_mesh_data->indices.size(), GL_UNSIGNED_INT, 0,  (int)matModel.size());
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glDisable(GL_POLYGON_OFFSET_LINE);
        glPolygonOffset(0.0f, 0.0f);
    }
    glBindVertexArray(0);

    return true;
}

void MeshPass::setup(const std::vector<Matrix> &matModel, bool hasTexture, bool hasAni) {
    if (_VBO == 0) {
        glGenVertexArrays(1, &_VAO);
        glGenBuffers(1, &_VBO);
        glGenBuffers(1, &_EBO);
    }
    
    glBindVertexArray(_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, _VBO);
    
    
    size_t size1 = m_mesh_data->positions.size() * sizeof(glm::vec3);
    size_t size2 = m_mesh_data->normals.size() * sizeof(glm::vec3);
    size_t size3 = m_mesh_data->boneIDs.size() * sizeof(glm::ivec4);
    size_t size4 = m_mesh_data->weights.size() * sizeof(glm::vec4);
    size_t totalsize = size1 + size2;
    if (hasAni) {
        totalsize += (size3 + size4);
    }
    size_t size5 = sizeof(Matrix) * matModel.size();
    totalsize += size5;
    
    glBufferData(GL_ARRAY_BUFFER, totalsize, nullptr , GL_STATIC_DRAW);
    glBufferSubData(GL_ARRAY_BUFFER, 0, size1, m_mesh_data->positions.data());
    glBufferSubData(GL_ARRAY_BUFFER, size1, size2, m_mesh_data->normals.data());
    
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)0);
    glEnableVertexAttribArray(0);
    
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), (void*)size1);
    glEnableVertexAttribArray(1);
    
    int bufferIndex = 2;
    if (hasAni) {
        glBufferSubData(GL_ARRAY_BUFFER, size1 + size2, size3, m_mesh_data->boneIDs.data());
        glBufferSubData(GL_ARRAY_BUFFER, size1 + size2 + size3, size4, m_mesh_data->weights.data());
        
        glEnableVertexAttribArray(2);
        glVertexAttribPointer(2, 4, GL_INT, GL_FALSE, sizeof(glm::ivec4), (void*)(size1 + size2));
        
        glEnableVertexAttribArray(3);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(glm::vec4), (void*)(size1 + size2 + size3));
        bufferIndex += 2;
    }
    
    glBufferSubData(GL_ARRAY_BUFFER, totalsize - size5, size5, matModel.data());
    glVertexAttribPointer(bufferIndex, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(totalsize - size5));
    glVertexAttribDivisor(bufferIndex, 1);
    glEnableVertexAttribArray(bufferIndex++);
    
    glVertexAttribPointer(bufferIndex, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(totalsize - size5 + sizeof(glm::vec4)));
    glVertexAttribDivisor(bufferIndex, 1);
    glEnableVertexAttribArray(bufferIndex++);
    
    glVertexAttribPointer(bufferIndex, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(totalsize - size5 + 2 * sizeof(glm::vec4)));
    glVertexAttribDivisor(bufferIndex, 1);
    glEnableVertexAttribArray(bufferIndex++);
    
    glVertexAttribPointer(bufferIndex, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(totalsize - size5 + 3 * sizeof(glm::vec4)));
    glVertexAttribDivisor(bufferIndex, 1);
    glEnableVertexAttribArray(bufferIndex++);

    
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, m_mesh_data->indices.size() * sizeof(unsigned int), m_mesh_data->indices.data(), GL_STATIC_DRAW);
    
    glBindVertexArray(0);
}


// ================= M4.2: RHI 绘制路径 =================
#include <glm/gtc/type_ptr.hpp>

void MeshPass::ensureRHIResources(rhi::IRHIDevice* device) {
    if (m_rhiBuilt) return;

    // 合批前置：同 MeshData 的几何资源全局共享（VB/IB/管线/纹理）。
    // 实例缓冲与绘制级 uniform block 是每对象状态，保持独立。
    // 键 = MeshData 指针（解析缓存保证同文件模型的 MeshData 稳定共享）。
    struct SharedRhiGeom {
        rhi::BufferHandle vb, ib;
        rhi::PipelineHandle pipeline;
        rhi::GraphicsPipelineDesc desc;
        bool valid = false;
    };
    static std::map<const MeshData*, SharedRhiGeom> s_geomCache;
    static std::map<std::string, rhi::TextureHandle> s_texCache;   // 材质纹理按文件路径共享

    auto it = s_geomCache.find(m_mesh_data.get());
    if (it != s_geomCache.end() && it->second.valid) {
        const SharedRhiGeom& g = it->second;
        m_rhiVB = g.vb;
        m_rhiIB = g.ib;
        m_rhiPipeline = g.pipeline;
        _lastPipelineDesc = g.desc;
        m_rhiInstanceVB = device->createBuffer([] {
            rhi::BufferDesc instDesc;
            instDesc.size = 64 * 4;   // 初始 4 实例；每帧 updateBuffer 刷新
            instDesc.usage = rhi::BUF_CPU_WRITE;
            return instDesc;
        }());
        m_rhiUniformBlock = device->createUniformBlock(256);
        m_rhiBuilt = true;
        return;
    }

    // 交错顶点数据：pos(3) + normal(3) + uv(2)
    const auto& md = m_mesh_data;
    const size_t vn = md->positions.size();
    std::vector<float> verts;
    verts.reserve(vn * 8);
    for (size_t i = 0; i < vn; ++i) {
        const auto& p = md->positions[i];
        const auto& n = md->normals[i];
        const auto& t = md->coords[i];
        verts.push_back(p.x); verts.push_back(p.y); verts.push_back(p.z);
        verts.push_back(n.x); verts.push_back(n.y); verts.push_back(n.z);
        verts.push_back(t.x); verts.push_back(t.y);
    }

    rhi::GraphicsPipelineDesc desc;
    desc.shaders = {
        { rhi::ShaderStage::Vertex,   isTextured() ? "vert_model" : "vert_model_color" },
        { rhi::ShaderStage::Fragment, isTextured() ? "frag_model" : "frag_model_color" },
    };
    desc.vertexLayout.stride = 8 * sizeof(float);          // pos3+normal3+uv2
    desc.vertexLayout.instanceStride = sizeof(Matrix);     // 实例矩阵 mat4
    desc.vertexLayout.attributes = {
        { 0, rhi::VertexFormat::Float3, 0,  false },
        { 1, rhi::VertexFormat::Float3, 12, false },
        { 2, rhi::VertexFormat::Float2, 24, false },
        { 3, rhi::VertexFormat::Float4, 0,  true },    // 实例矩阵 4 行
        { 4, rhi::VertexFormat::Float4, 16, true },
        { 5, rhi::VertexFormat::Float4, 32, true },
        { 6, rhi::VertexFormat::Float4, 48, true },
    };
    desc.primitive = rhi::PrimitiveType::Triangles;
    desc.cull      = rhi::CullMode::Back;
    desc.depth     = { true, true };
    _lastPipelineDesc = desc;   // D3：blend 变体的复制母本
    m_rhiPipeline = device->createPipeline(desc);

    rhi::BufferDesc vbDesc;
    vbDesc.size = static_cast<uint32_t>(verts.size() * sizeof(float));
    vbDesc.usage = rhi::BUF_VERTEX;
    vbDesc.initialData = verts.data();
    m_rhiVB = device->createBuffer(vbDesc);

    // 实例缓冲：按当前实例数创建容量，每帧 updateBuffer 刷新
    rhi::BufferDesc instDesc;
    instDesc.size = 64 * 4;   // 初始 4 实例（Model::setCount(4)）；超容见 docs 已知限制
    instDesc.usage = rhi::BUF_CPU_WRITE;
    m_rhiInstanceVB = device->createBuffer(instDesc);

    rhi::BufferDesc ibDesc;
    ibDesc.size = static_cast<uint32_t>(md->indices.size() * sizeof(unsigned int));
    ibDesc.usage = rhi::BUF_INDEX;
    ibDesc.initialData = md->indices.data();
    m_rhiIB = device->createBuffer(ibDesc);

    m_rhiUniformBlock = device->createUniformBlock(256);

    SharedRhiGeom& g = s_geomCache[m_mesh_data.get()];
    g.vb = m_rhiVB;
    g.ib = m_rhiIB;
    g.pipeline = m_rhiPipeline;
    g.desc = desc;
    g.valid = true;
    m_rhiBuilt = true;
}

bool MeshPass::DrawRHI(const std::vector<Matrix> &matModel, const Matrix &mtx, uint32_t flags, float opacity) {
    auto& rs = RenderSystem::getInstance();
    auto* device = rs.rhiDevice();
    if (!device) return false;

    // M4.2 限制：动画 / 阴影 / 拾取 / 多视口暂未迁移
    if (m_matBone && !m_matBone->empty()) {
        ENGINE_LOG_WARN_ONCE("[RHI] animated model not migrated yet (M4.3)");
        return false;
    }
    if (flags & (DrawOption::DRAW_SHADOW | DrawOption::DRAW_PICK | DrawOption::MULTI_VIEWPORT)) {
        ENGINE_LOG_WARN_ONCE("[RHI] shadow/pick/multiview paths not migrated yet (M4.3)");
        return false;
    }
    if (!isTextured()) {
        ENGINE_LOG_WARN_ONCE("[RHI] color model pipeline not migrated yet (M4.3)");
        return false;
    }

    ensureRHIResources(device);
    if (!m_rhiPipeline.valid() || !m_rhiVB.valid() || !m_rhiIB.valid()) return false;
    const auto& md = m_mesh_data;

    auto* cmd = device->commandList();

    // 实例矩阵刷新（容量保护：超出初始容量不写入）
    const uint32_t instBytes = static_cast<uint32_t>(matModel.size() * sizeof(Matrix));
    if (matModel.size() * sizeof(Matrix) <= 64 * 4) {
        device->updateBuffer(m_rhiInstanceVB, matModel.data(), instBytes);
    } else {
        ENGINE_LOG_WARN_ONCE("[RHI] instance count exceeds buffer capacity");
        return false;
    }

    // D5：显示模式（着色 / 线框 / 着色+边线）
    const int displayMode = rs.settings().displayMode;

    // D3：透明对象走 blend 变体（depthWrite 关 + srcAlpha 混合；管线惰性建，
    // 附件格式与 opaque 一致——B4 契约的 swapchain 兼容约束）
    const bool transparent = (opacity < 0.999f);
    if (transparent && !m_rhiPipelineBlend.valid()) {
        rhi::GraphicsPipelineDesc bdesc = _lastPipelineDesc;
        bdesc.blend.blendEnable = true;
        bdesc.depth.depthWrite  = false;
        m_rhiPipelineBlend = device->createPipeline(bdesc);
        if (!m_rhiPipelineBlend.valid()) {
            ENGINE_LOG_WARN_ONCE("[RHI] blend pipeline creation failed; falls back to opaque");
        }
    }
    const rhi::PipelineHandle pipe = (transparent && m_rhiPipelineBlend.valid())
                                         ? m_rhiPipelineBlend : m_rhiPipeline;

    // D5：线框管线（solid 着色器 + PolygonMode::Line，惰性建）
    const bool wireNeeded = (displayMode != 0);
    if (wireNeeded && !m_rhiPipelineWire.valid()) {
        rhi::GraphicsPipelineDesc wdesc;
        wdesc.shaders = {
            { rhi::ShaderStage::Vertex,   "vert_wireframe_rhi" },
            { rhi::ShaderStage::Fragment, "frag_wireframe_rhi" },
        };
        wdesc.vertexLayout = _lastPipelineDesc.vertexLayout;   // 复用模型 VB 布局
        wdesc.primitive    = rhi::PrimitiveType::Triangles;
        wdesc.cull         = rhi::CullMode::None;              // 线框不剔除
        wdesc.depth        = { true, displayMode == 1 };       // 着色+边线：深度偏移写深度
        wdesc.blend        = { false };
        wdesc.raster.polygonMode           = rhi::PolygonMode::Line;
        wdesc.raster.depthBiasConstantFactor = (displayMode == 2) ? 2.0f : 0.0f;
        wdesc.raster.depthBiasSlopeFactor    = (displayMode == 2) ? 2.0f : 0.0f;
        m_rhiPipelineWire = device->createPipeline(wdesc);
        if (!m_rhiPipelineWire.valid()) {
            ENGINE_LOG_WARN_ONCE("[RHI] wireframe pipeline creation failed; falls back to shaded");
        }
    }

    glm::mat4 vp = Matrix::toMatrix(mtx);

    if (displayMode == 1 && m_rhiPipelineWire.valid()) {
        // ---- 纯线框 ----
        cmd->bindPipeline(m_rhiPipelineWire);
        cmd->setUniform("uViewProj", rhi::UniformValue::fromMat4(glm::value_ptr(vp)));
        const glm::vec4 wc = wireColorOf();
        cmd->setUniform("uColor", rhi::UniformValue::fromVec4(glm::value_ptr(wc)));
        device->updateBuffer(m_rhiInstanceVB, matModel.data(),
                             std::min<uint32_t>(static_cast<uint32_t>(matModel.size() * sizeof(Matrix)), 64 * 4));
        cmd->bindVertexBuffer(m_rhiVB);
        if (m_rhiInstanceVB.valid()) cmd->bindVertexBuffer(m_rhiInstanceVB, 1);
        cmd->bindIndexBuffer(m_rhiIB);
        cmd->drawIndexed(static_cast<uint32_t>(md->indices.size()),
                         static_cast<uint32_t>(matModel.size()));
        return true;
    }

    cmd->bindPipeline(pipe);

    // uViewProj → push constant offset 0
    cmd->setUniform("uViewProj", rhi::UniformValue::fromMat4(glm::value_ptr(vp)));

    // D7：剖切平面 → push constant offset 64
    {
        const auto& settings = rs.settings();
        glm::vec4 clipPlane = settings.clipPlaneEnabled
            ? glm::vec4(settings.clipPlaneNormal[0], settings.clipPlaneNormal[1],
                        settings.clipPlaneNormal[2], settings.clipPlaneDistance)
            : glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
        cmd->setUniform("uClipPlane", rhi::UniformValue::fromVec4(glm::value_ptr(clipPlane)));
    }

    // D8：选中高亮 → push constant offset 80（B6 反射表按名定位）
    {
        const glm::vec4 hl = highlightValueOf(flags);
        cmd->setUniform("uHighlight", rhi::UniformValue::fromVec4(glm::value_ptr(hl)));
    }
    {
        const glm::vec4 hv = hoverValueOf(flags);
        cmd->setUniform("uHover", rhi::UniformValue::fromVec4(glm::value_ptr(hv)));
    }

    // 绘制级 uniform（灯光/材质/相机）
    engine::DrawUniforms du{};
    auto light = Light::GlobalLight();
    du.lightPosition_isOpen = glm::vec4(light.position,
                                        (flags & DrawOption::LIGHT_OPEN) ? 1.0f : 0.0f);
    du.lightDirection_cosTheta = glm::vec4(light.direction, light.cosTheta);
    du.lightAmbient = glm::vec4(light.ambient, 0);
    du.lightDiffuse = glm::vec4(light.diffuse, 0);
    du.lightSpecular = glm::vec4(light.specular, 0);
    auto camPos = Camera::GetCamera().getPossition();
    du.cameraPos = glm::vec4(camPos, 0);
    du.materialAmbient = m_material->ambient;      // Material 字段本身为 vec4
    du.materialDiffuse = m_material->diffuse;
    du.materialSpecular = m_material->specular;
    bool hasNormalTex = false, hasSpecularTex = false;
    du.materialParams = glm::vec4(m_material->shininess, m_material->shininess_strength, 0, 0);
    du.misc = glm::vec4(opacity, 0.0f, 0.0f, 0.0f);   // D3：不透明度（首次提交，纹理标记稍后重提）
    cmd->setDrawUniforms(m_rhiUniformBlock, &du, sizeof(du));

    // 材质纹理：槽位与 GL 路径一致（按材质纹理顺序即 GL 的 texture unit 序）
    int slot = 0;
    for (const auto& tex : m_material->textures) {
        if (slot >= 4) break;
        if (tex.name == "texture_normal0" && md->tangents.empty()) {
            // GL 路径同样不创建无切线的法线纹理绑定
            continue;
        }
        rhi::TextureHandle th;
        // 合批前置：材质纹理按路径全局共享（同文件模型的纹理只建一次）
        static std::map<std::string, rhi::TextureHandle> s_rhiTexShared;
        auto it = s_rhiTexShared.find(tex.filepath);
        if (it != s_rhiTexShared.end()) {
            th = it->second;
        } else {
            // 纹理数据源：优先材质自带数据；为空则取 TextureMng 预载数据
            // （模型外部纹理如 duckCM.tga 由 preloadTextures 递归加载）
            int tw = 0, thh = 0, ch = 0;
            ImageBuffer data = tex.data;
            if (!data || data->empty()) {
                data = TextureMng::getInstance().getTextureData(tex.filepath, tw, thh, ch);
            } else {
                tw = tex.width; thh = tex.height; ch = tex.format;
            }
            if (data && !data->empty() && tw > 0 && thh > 0) {
                // 转换为 RGBA8（VK createTexture 按 4 通道上传）
                // 纵向翻转：stb 加载为 top-first 行序，而 UV 约定 v=0 为底部
                //（RHI 路径上传不翻转会导致鸭子采样到纹理顶部黑色眼睛区域）
                std::vector<unsigned char> rgba;
                const size_t px = static_cast<size_t>(tw) * thh;
                rgba.resize(px * 4);
                const bool flipV = true;
                for (int y = 0; y < thh; ++y) {
                    const int sy = flipV ? (thh - 1 - y) : y;
                    for (int x = 0; x < tw; ++x) {
                        const size_t so = (static_cast<size_t>(sy) * tw + x) * ch;
                        const size_t doo = (static_cast<size_t>(y) * tw + x) * 4;
                        if (ch == 4) {
                            rgba[doo+0] = (*data)[so+0];
                            rgba[doo+1] = (*data)[so+1];
                            rgba[doo+2] = (*data)[so+2];
                            rgba[doo+3] = (*data)[so+3];
                        } else if (ch == 3) {
                            rgba[doo+0] = (*data)[so+0];
                            rgba[doo+1] = (*data)[so+1];
                            rgba[doo+2] = (*data)[so+2];
                            rgba[doo+3] = 255;
                        } else {
                            rgba[doo+0] = rgba[doo+1] = rgba[doo+2] = 255;
                            rgba[doo+3] = 255;
                        }
                    }
                }
                rhi::TextureDesc td;
                td.width = tw; td.height = thh;
                td.usage = rhi::TEX_SAMPLED;
                // B3c：颜色贴图 → sRGB（硬件解码）；法线/高度 → 线性数据
                const bool colorTex = tex.name.rfind("texture_diffuse", 0) == 0
                                   || tex.name.rfind("texture_specular", 0) == 0;
                td.format = colorTex ? rhi::Format::RGBA8_SRGB : rhi::Format::RGBA8_UNorm;
                td.initialData = rgba.data();
                th = device->createTexture(td);
                s_rhiTexShared[tex.filepath] = th;
            }
        }
        if (th.valid()) {
            cmd->bindTexture(th, slot);
            if (tex.name == "texture_normal0")   hasNormalTex = true;
            if (tex.name == "texture_specular0") hasSpecularTex = true;
        }
        slot++;
    }
    du.materialParams = glm::vec4(m_material->shininess, m_material->shininess_strength,
                                  hasNormalTex ? 1.0f : 0.0f, hasSpecularTex ? 1.0f : 0.0f);
    du.misc = glm::vec4(opacity, 0.0f, 0.0f, 0.0f);   // D3：重提保持 opacity
    cmd->setDrawUniforms(m_rhiUniformBlock, &du, sizeof(du));   // 重提（含纹理标记）

    cmd->bindVertexBuffer(m_rhiVB);
    if (m_rhiInstanceVB.valid()) {
        cmd->bindVertexBuffer(m_rhiInstanceVB, 1);
    }
    cmd->bindIndexBuffer(m_rhiIB);
    cmd->drawIndexed(static_cast<uint32_t>(md->indices.size()),
                     static_cast<uint32_t>(matModel.size()));

    // D5：着色+边线 —— 第二遍线框（深度偏移已在管线里，防止 z-fighting）
    if (displayMode == 2 && m_rhiPipelineWire.valid()) {
        cmd->bindPipeline(m_rhiPipelineWire);
        cmd->setUniform("uViewProj", rhi::UniformValue::fromMat4(glm::value_ptr(vp)));
        const glm::vec4 wc = wireColorOf();
        cmd->setUniform("uColor", rhi::UniformValue::fromVec4(glm::value_ptr(wc)));
        // 实例缓冲已刷新，直接重画同 VB/IB
        cmd->bindVertexBuffer(m_rhiVB);
        if (m_rhiInstanceVB.valid()) cmd->bindVertexBuffer(m_rhiInstanceVB, 1);
        cmd->bindIndexBuffer(m_rhiIB);
        cmd->drawIndexed(static_cast<uint32_t>(md->indices.size()),
                         static_cast<uint32_t>(matModel.size()));
    }
    return true;
}

// ================= D2: RHI 拾取绘制 =================
// pick 管线（vert_pick_rhi/frag_pick_rhi）：ID 编码色经 push constant 直出，
// 无光照/纹理/FrameData。顶点布局与绘制管线一致（同一套 VB/IB/实例缓冲复用）。
bool MeshPass::DrawShadowRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle shadowPipeline,
                             const Matrix& lightVP, const std::vector<Matrix>& matModel) {
    auto& rs = RenderSystem::getInstance();
    auto* device = rs.rhiDevice();
    if (!device || !cmd || !shadowPipeline.valid()) return false;

    ensureRHIResources(device);
    if (!m_rhiVB.valid() || !m_rhiIB.valid()) return false;
    const auto& md = m_mesh_data;

    const uint32_t instBytes = static_cast<uint32_t>(matModel.size() * sizeof(Matrix));
    if (matModel.size() * sizeof(Matrix) > 64 * 4) {
        ENGINE_LOG_WARN_ONCE("[RHI] shadow: instance count exceeds buffer capacity");
        return false;
    }
    device->updateBuffer(m_rhiInstanceVB, matModel.data(), instBytes);

    cmd->bindPipeline(shadowPipeline);
    // 矩阵已在渲染侧做过 Reversed-Z 修正（与深度比较约定一致）
    glm::mat4 lvp = Matrix::toMatrix(lightVP);
    cmd->setUniform("uLightMat", rhi::UniformValue::fromMat4(glm::value_ptr(lvp)));

    cmd->bindVertexBuffer(m_rhiVB, 0);
    cmd->bindVertexBuffer(m_rhiInstanceVB, 1);
    cmd->bindIndexBuffer(m_rhiIB);
    cmd->drawIndexed(static_cast<uint32_t>(md->indices.size()),
                     static_cast<uint32_t>(matModel.size()));
    return true;
}

bool MeshPass::DrawPickRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle pickPipeline,
                           const Matrix& viewProj, const std::vector<Matrix>& matModel,
                           uint16_t pickID) {
    auto& rs = RenderSystem::getInstance();
    auto* device = rs.rhiDevice();
    if (!device || !cmd || !pickPipeline.valid()) return false;

    ensureRHIResources(device);
    if (!m_rhiVB.valid() || !m_rhiIB.valid()) return false;
    const auto& md = m_mesh_data;

    // 实例矩阵刷新（与 DrawRHI 相同的容量保护）
    const uint32_t instBytes = static_cast<uint32_t>(matModel.size() * sizeof(Matrix));
    if (matModel.size() * sizeof(Matrix) > 64 * 4) {
        ENGINE_LOG_WARN_ONCE("[RHI] pick: instance count exceeds buffer capacity");
        return false;
    }
    device->updateBuffer(m_rhiInstanceVB, matModel.data(), instBytes);

    cmd->bindPipeline(pickPipeline);

    // uViewProj → push constant offset 0
    glm::mat4 vp = Matrix::toMatrix(viewProj);
    cmd->setUniform("uViewProj", rhi::UniformValue::fromMat4(glm::value_ptr(vp)));

    // ID 编码 → uPickColor。pick RT 是 sRGB 格式（BGRA8_SRGB，与管线烘焙格式兼容），
    // 写入会做 linear→sRGB 编码：传 srgbToLinear(byte/255) 才能在 8bit 精度下
    // 精确还原（byte→linear→byte 恒等）；alpha 不编码，1.0 直存 255。
    const float r = color::srgbToLinear(static_cast<float>(pickID & 0xFF) / 255.0f);
    const float g = color::srgbToLinear(static_cast<float>((pickID >> 8) & 0xFF) / 255.0f);
    const float pickColor[4] = { r, g, 0.0f, 1.0f };
    cmd->setUniform("uPickColor", rhi::UniformValue::fromVec4(pickColor));

    cmd->bindVertexBuffer(m_rhiVB, 0);
    cmd->bindVertexBuffer(m_rhiInstanceVB, 1);
    cmd->bindIndexBuffer(m_rhiIB);
    cmd->drawIndexed(static_cast<uint32_t>(md->indices.size()),
                     static_cast<uint32_t>(matModel.size()));
    return true;
}
