//
//  pass.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/22.
//

#include "mesh_pass.h"
#include <unordered_map>
#include <cmath>
#include <cstdint>
#include <set>
#include <mutex>
#include "RenderSystem.h"
#include "rhi/IRHIDevice.h"
#include "rhi/IRHICommandList.h"
#include "render/DrawUniforms.h"
#include "color/ColorSpace.h"
#include "Light.h"
#include "../scene.h"
#include "../Light.h"
#include "image.h"

static bool mathDataHasTexture(const std::shared_ptr<Material>& mat) {
    return mat && !mat->textures.empty();
}

MeshPass::MeshPass(std::shared_ptr<MeshData> meshData, std::shared_ptr<Material> matData) {
    m_mesh_data = meshData;
    m_material = matData;
    m_textured = mathDataHasTexture(matData);
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

bool MeshPass::Draw(rhi::IRHICommandList* cmd, const std::vector<Matrix> &matModel, const Matrix &mtx, uint32_t flags,
                    int /*numViewpoit*/, float opacity) {
    // 去后端化：GL/VK 同一 RHI 路径。多视口由逐 tile viewport 重绘实现，
    // 旧着色器的 MULTI_VIEWPORT 标志在 RHI 路径不再需要。
    return DrawRHI(cmd, matModel, mtx, flags & ~DrawOption::MULTI_VIEWPORT, opacity);
}

// ================= M4.2: RHI 绘制路径 =================
#include <glm/gtc/type_ptr.hpp>

void MeshPass::ensureRHIResources(rhi::IRHIDevice* device) {
    if (m_rhiBuilt) return;
    // 多线程录制：首次创建走共享缓存，加锁避免 s_geomCache/s_texCache 竞争
    static std::mutex s_rhiResMutex;
    std::lock_guard<std::mutex> lock(s_rhiResMutex);
    if (m_rhiBuilt) return;   // double-check

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
        { rhi::ShaderStage::Vertex,   m_textured ? "vert_model" : "vert_model_color" },
        { rhi::ShaderStage::Fragment, m_textured ? "frag_model" : "frag_model_color" },
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

bool MeshPass::DrawRHI(rhi::IRHICommandList* cmd, const std::vector<Matrix> &matModel, const Matrix &mtx, uint32_t flags, float opacity) {
    auto& rs = RenderSystem::getInstance();
    auto* device = rs.rhiDevice();
    if (!device) return false;

    // M4.2 限制：动画 / 阴影 / 拾取 / 多视口暂未迁移
    if (m_matBone && !m_matBone->empty()) {
        ENGINE_LOG_WARN_ONCE("[RHI] animated model not migrated yet (M4.3)");
        return false;
    }
    if (flags & (DrawOption::DRAW_SHADOW | DrawOption::DRAW_PICK)) {
        ENGINE_LOG_WARN_ONCE("[RHI] shadow/pick use dedicated entry points (DrawShadowRHI/DrawPickRHI)");
        return false;
    }

    ensureRHIResources(device);
    if (!m_rhiPipeline.valid() || !m_rhiVB.valid() || !m_rhiIB.valid()) return false;
    const auto& md = m_mesh_data;

    // E4：LOD 选择——相机到本对象第一个实例的距离超过阈值时换简化网格
    rhi::BufferHandle drawVB = m_rhiVB;
    rhi::BufferHandle drawIB = m_rhiIB;
    uint32_t drawIndexCount = static_cast<uint32_t>(md->indices.size());
    {
        const auto& st = rs.settings();
        if (st.lodEnabled && !matModel.empty()) {
            const float* m0 = matModel[0].buffer();
            const glm::vec3 center(m0[12], m0[13], m0[14]);
            const glm::vec3 cam = rs.activeCameraPos();
            const glm::vec3 d = center - cam;
            const float dist = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
            if (dist > st.lodDistance
                && ensureLodGeometry(device)) {
                drawVB = m_rhiVB1;
                drawIB = m_rhiIB1;
                drawIndexCount = m_lod1Tris * 3;
            }
        }
    }

    // cmd 由调用方传入（多线程录制：每批对象录到各自的命令列表）

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
    bool hasNormalTex = false, hasMetalroughTex = false, hasAoTex = false;
    du.materialParams = glm::vec4(m_material->metallic, m_material->roughness, 0, m_material->ao);
    // V3-19：阴影——光空间矩阵 + 采样开关（y=1 才采样，没阴影贴图时保持原样）
    const rhi::TextureHandle shadowTex = rs.shadowTexture();
    du.lightVP = Matrix::toMatrix(rs.shadowLightVP());
    du.misc = glm::vec4(opacity, shadowTex.valid() ? 1.0f : 0.0f, 0.0f, 0.0f);
    cmd->setDrawUniforms(m_rhiUniformBlock, &du, sizeof(du));

    // 材质纹理：槽位按语义固定（albedo=0, metalrough/specular=1, normal=2, ao=3）
    for (const auto& tex : m_material->textures) {
        int slot = -1;
        if (tex.name == "texture_diffuse0")       slot = 0;
        else if (tex.name == "texture_specular0" || tex.name == "texture_metalrough0") slot = 1;
        else if (tex.name == "texture_normal0")   slot = 2;
        else if (tex.name == "texture_ao0")       slot = 5;
        if (slot < 0) continue;
        if (tex.name == "texture_normal0" && md->tangents.empty()) {
            // GL 路径同样不创建无切线的法线纹理绑定
            continue;
        }
        rhi::TextureHandle th;
        // 合批前置：材质纹理按路径全局共享（同文件模型的纹理只建一次）
        static std::map<std::string, rhi::TextureHandle> s_rhiTexShared;
        static std::mutex s_texSharedMutex;
        auto it = s_rhiTexShared.find(tex.filepath);
        if (it != s_rhiTexShared.end()) {
            th = it->second;
        } else {
            std::lock_guard<std::mutex> lock(s_texSharedMutex);   // 多线程：写路径加锁
            it = s_rhiTexShared.find(tex.filepath);              // double-check
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
        }
        if (th.valid()) {
            cmd->bindTexture(th, slot, tex.name.c_str());
            if (tex.name == "texture_normal0")     hasNormalTex = true;
            if (tex.name == "texture_metalrough0") hasMetalroughTex = true;
            if (tex.name == "texture_ao0")         hasAoTex = true;
        }
    }
    du.materialParams = glm::vec4(m_material->metallic, m_material->roughness,
                                  hasNormalTex ? 1.0f : 0.0f, m_material->ao);
    // V3-19：重提时保持阴影开关（与首次提交一致）
    du.misc = glm::vec4(opacity, rs.shadowTexture().valid() ? 1.0f : 0.0f,
                        hasMetalroughTex ? 1.0f : 0.0f, hasAoTex ? 1.0f : 0.0f);
    cmd->setDrawUniforms(m_rhiUniformBlock, &du, sizeof(du));   // 重提（含纹理标记）

    // V3-19：阴影贴图固定占槽 4（set5），材质纹理只用槽 0..3
    if (shadowTex.valid()) {
        cmd->bindTexture(shadowTex, 4, "uShadowMap");
    }

    cmd->bindVertexBuffer(drawVB);
    if (m_rhiInstanceVB.valid()) {
        cmd->bindVertexBuffer(m_rhiInstanceVB, 1);
    }
    cmd->bindIndexBuffer(drawIB);
    cmd->drawIndexed(drawIndexCount,
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
// E4：顶点聚簇简化——按格子把顶点合并成一个代表点，去掉退化三角形。
// 输入输出都是 RHI 的交错布局（pos3+normal3+uv2）。
static void clusterDecimate(const MeshData& md, float gridScale,
                            std::vector<float>& outVerts,
                            std::vector<unsigned int>& outIndices) {
    const size_t vn = md.positions.size();
    outVerts.clear();
    outIndices.clear();
    if (vn == 0 || md.indices.empty()) return;

    glm::vec3 lo = md.positions[0], hi = md.positions[0];
    for (const auto& p : md.positions) {
        lo = glm::min(lo, p);
        hi = glm::max(hi, p);
    }
    const glm::vec3 extent = hi - lo;
    const float diag = std::sqrt(extent.x * extent.x + extent.y * extent.y + extent.z * extent.z);
    const float grid = std::max(diag * gridScale, 1e-5f);
    const float inv = 1.0f / grid;

    std::unordered_map<uint64_t, uint32_t> cellToNew;
    cellToNew.reserve(vn);
    std::vector<uint32_t> remap(vn, 0);

    auto cellKey = [inv](const glm::vec3& p, int32_t& x, int32_t& y, int32_t& z) {
        x = static_cast<int32_t>(std::floor(p.x * inv));
        y = static_cast<int32_t>(std::floor(p.y * inv));
        z = static_cast<int32_t>(std::floor(p.z * inv));
    };

    for (size_t i = 0; i < vn; ++i) {
        int32_t cx, cy, cz;
        cellKey(md.positions[i], cx, cy, cz);
        const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx) & 0x1FFFFF) << 42)
                           | (static_cast<uint64_t>(static_cast<uint32_t>(cy) & 0x1FFFFF) << 21)
                           | static_cast<uint64_t>(static_cast<uint32_t>(cz) & 0x1FFFFF);
        auto it = cellToNew.find(key);
        if (it != cellToNew.end()) {
            remap[i] = it->second;
            continue;
        }
        const uint32_t idx = static_cast<uint32_t>(outVerts.size() / 8);
        cellToNew.emplace(key, idx);
        remap[i] = idx;
        const glm::vec3& p = md.positions[i];
        const glm::vec3 n = i < md.normals.size() ? md.normals[i] : glm::vec3(0, 1, 0);
        const glm::vec2 t = i < md.coords.size() ? md.coords[i] : glm::vec2(0);
        outVerts.push_back(p.x); outVerts.push_back(p.y); outVerts.push_back(p.z);
        outVerts.push_back(n.x); outVerts.push_back(n.y); outVerts.push_back(n.z);
        outVerts.push_back(t.x); outVerts.push_back(t.y);
    }

    outIndices.reserve(md.indices.size());
    for (size_t i = 0; i + 2 < md.indices.size(); i += 3) {
        const uint32_t a = remap[md.indices[i]];
        const uint32_t b = remap[md.indices[i + 1]];
        const uint32_t c = remap[md.indices[i + 2]];
        if (a == b || b == c || a == c) continue;   // 退化三角形
        outIndices.push_back(a);
        outIndices.push_back(b);
        outIndices.push_back(c);
    }
}

bool MeshPass::ensureLodGeometry(rhi::IRHIDevice* device) {
    if (m_lodBuilt) return m_rhiVB1.valid() && m_rhiIB1.valid();
    m_lodBuilt = true;
    const auto& md = m_mesh_data;
    if (!md || md->indices.empty()) return false;

    std::vector<float> verts;
    std::vector<unsigned int> idx;
    clusterDecimate(*md, RenderSystem::getInstance().settings().lodRatio, verts, idx);
    if (idx.empty() || verts.empty()) return false;

    rhi::BufferDesc vb;
    vb.size = static_cast<uint32_t>(verts.size() * sizeof(float));
    vb.usage = rhi::BUF_VERTEX;
    vb.initialData = verts.data();
    m_rhiVB1 = device->createBuffer(vb);

    rhi::BufferDesc ib;
    ib.size = static_cast<uint32_t>(idx.size() * sizeof(unsigned int));
    ib.usage = rhi::BUF_INDEX;
    ib.initialData = idx.data();
    m_rhiIB1 = device->createBuffer(ib);

    m_lod0Tris = static_cast<uint32_t>(md->indices.size() / 3);
    m_lod1Tris = static_cast<uint32_t>(idx.size() / 3);
    ENGINE_LOG_INFO("[LOD] mesh tris %u -> %u (%.0f%%)",
                    m_lod0Tris, m_lod1Tris,
                    m_lod0Tris ? (m_lod1Tris * 100.0f / m_lod0Tris) : 0.0f);
    return m_rhiVB1.valid() && m_rhiIB1.valid();
}

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
