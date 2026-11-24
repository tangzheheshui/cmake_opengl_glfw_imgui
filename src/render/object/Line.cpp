//
//  Line.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/25.
//
//  M2: 增加 RHI 绘制路径（与原 GL 路径并存，调试面板 "Line 走 RHI" 开关切换）
//  对比原 GL 路径的两个差异：
//  1. 数据只在变化时上传（原路径 calculate() 每帧 glBufferData 重传）
//  2. 渲染状态由 Pipeline 描述符声明（LINES / 深度测试开 / 无裁剪）
//
//  D1: RHI 路径改为宽线（三角形展开）——VK 硬限 wideLines 不可用、GL glLineWidth
//  质量不可靠，改为 CPU 端把每条线段展开成 2 个三角形（4 顶点 6 索引），
//  顶点属性携带端点 A/B + 展开参数（端别/侧别/宽度像素）+ 像素→NDC 换算，
//  顶点着色器投影后在屏幕（像素）空间做法向扩展（详见 res/shaders/vert_line_rhi）。

#include "Line.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include "../shader.h"
#include "../RenderSystem.h"
#include "../scene.h"
#include "rhi/IRHIDevice.h"
#include "rhi/IRHICommandList.h"
#include "../camera/camera.h"

void Line::setData(const std::vector<glm::vec3> &points, const std::vector<unsigned int> &indices) {
    m_points = points;
    m_indices = indices;
    m_rhiDirty = true;   // RHI 路径：变化后由展开重建 + 上传
}

bool Line::draw(const Matrix &mtx) {

    // ---- RHI 路径：Vulkan 后端必须走（GL 路径依赖 ShaderCache，VK 模式未初始化）；
    //      GL 后端可通过调试面板 "Line 走 RHI" 开关切换对比 ----
    auto& renderSys = RenderSystem::getInstance();
    auto* device = renderSys.rhiDevice();
    if (device && (device->backend() == rhi::Backend::Vulkan
                   || renderSys.settings().lineUseRHI)) {
        return drawRHI(mtx);
    }

    // ---- 原 GL 路径（保持不变）----
    auto shader = ShaderCache::GetInstance().GetShader(ShaderType::Color);
    if (!shader) {
        return false;
    }

    shader->use();

    calculate();

    // 矩阵
    shader->setMat4("uMatrixMVP", mtx);

    // 颜色
    shader->setFloat3("uColor", m_color.r, m_color.g, m_color.b);

    glLineWidth(m_fWidth);
    glDrawElements(GL_LINES, (int)m_indices.size(), GL_UNSIGNED_INT, 0);
    return true;
}

void Line::refreshRhiViewport() {
    auto* device = RenderSystem::getInstance().rhiDevice();
    if (!device) return;
    if (device->backend() == rhi::Backend::OpenGL) {
        // GL：当前 GL 视口即像素真值（Pass 边界设满屏视口；C2 多视口时为当前 tile，
        // 宽线在 tile 内按 tile 尺寸换算，行为正确）。Vulkan 模式无 GL 上下文，
        // glad 函数指针为空，绝不进入本分支。
        GLint vp[4] = { 0, 0, 0, 0 };
        glGetIntegerv(GL_VIEWPORT, vp);
        m_rhiViewportW = (vp[2] > 0) ? static_cast<uint32_t>(vp[2]) : m_rhiViewportHintW;
        m_rhiViewportH = (vp[3] > 0) ? static_cast<uint32_t>(vp[3]) : m_rhiViewportHintH;
        return;
    }
    // Vulkan：B0 契约未提供视口/交换链尺寸查询，宽度换算以提示宽度为绝对基准，
    // 高度按当前相机纵横比（GetProjectMatrix 的 P11/P00）动态修正——
    // 窗口纵横比变化时展开方向不失真，仅绝对宽度按 实际W/提示W 等比缩放。
    m_rhiViewportW = m_rhiViewportHintW;
    float aspect = static_cast<float>(m_rhiViewportHintW) / static_cast<float>(m_rhiViewportHintH);
    if (auto scene = RenderSystem::getInstance().GetCurScene()) {
        if (auto cam = scene->GetActiveCamera()) {
            const glm::mat4 proj = Matrix::toMatrix(cam->GetProjectMatrix());
            if (proj[0][0] != 0.0f && proj[1][1] != 0.0f) {
                aspect = proj[1][1] / proj[0][0];   // perspective: P00 = P11 / aspect
            }
        }
    }
    if (aspect > 0.0f) {
        const float h = glm::max(1.0f, static_cast<float>(m_rhiViewportHintW) / aspect);
        m_rhiViewportH = static_cast<uint32_t>(h + 0.5f);
    } else {
        m_rhiViewportH = m_rhiViewportHintH;
    }
}

void Line::rebuildRhiExpandData() {
    m_rhiVerts.clear();
    m_rhiIndices.clear();
    const glm::vec2 screenScale(
        (m_rhiViewportW > 0) ? 2.0f / static_cast<float>(m_rhiViewportW) : 0.0f,
        (m_rhiViewportH > 0) ? 2.0f / static_cast<float>(m_rhiViewportH) : 0.0f);
    m_rhiVerts.reserve(m_indices.size() / 2 * 4);
    m_rhiIndices.reserve(m_indices.size() / 2 * 6);
    for (size_t i = 0; i + 1 < m_indices.size(); i += 2) {
        const unsigned int ia = m_indices[i];
        const unsigned int ib = m_indices[i + 1];
        if (ia >= m_points.size() || ib >= m_points.size()) continue;
        const glm::vec3 &A = m_points[ia];
        const glm::vec3 &B = m_points[ib];
        // 每段展开 4 顶点（端别 x 侧别），2 个三角形；cull=None，绕序无约束
        const unsigned int base = static_cast<unsigned int>(m_rhiVerts.size());
        const float sides[2] = { -1.0f, 1.0f };
        for (int end = 0; end < 2; ++end) {
            for (int s = 0; s < 2; ++s) {
                ExpandVertex v;
                v.posA = A;
                v.posB = B;
                v.meta = glm::vec4(static_cast<float>(end), sides[s], m_fWidth, 0.0f);
                v.screen = screenScale;
                m_rhiVerts.push_back(v);
            }
        }
        const unsigned int idx[6] = { base, base + 2, base + 1,   base + 1, base + 2, base + 3 };
        m_rhiIndices.insert(m_rhiIndices.end(), idx, idx + 6);
    }
}

void Line::uploadRhiExpandData(rhi::IRHIDevice* device) {
    const uint32_t vbBytes = static_cast<uint32_t>(m_rhiVerts.size() * sizeof(ExpandVertex));
    const uint32_t ibBytes = static_cast<uint32_t>(m_rhiIndices.size() * sizeof(unsigned int));
    if (!m_rhiVB.valid()) {
        rhi::BufferDesc vbDesc;
        vbDesc.size = vbBytes;
        vbDesc.usage = rhi::BUF_VERTEX;
        vbDesc.initialData = m_rhiVerts.data();
        m_rhiVB = device->createBuffer(vbDesc);

        rhi::BufferDesc ibDesc;
        ibDesc.size = ibBytes;
        ibDesc.usage = rhi::BUF_INDEX;
        ibDesc.initialData = m_rhiIndices.data();
        m_rhiIB = device->createBuffer(ibDesc);
        return;
    }
    // v1 契约：buffer 大小固定为首次创建时尺寸（见 docs/4-RHI接口契约.md），
    // setData 扩容超限时后端 updateBuffer 越界保护会拒绝（与原路径行为一致）
    if (vbBytes > 0 && ibBytes > 0) {
        device->updateBuffer(m_rhiVB, m_rhiVerts.data(), vbBytes);
        device->updateBuffer(m_rhiIB, m_rhiIndices.data(), ibBytes);
    }
}

bool Line::drawRHI(const Matrix &mtx) {
    auto* device = RenderSystem::getInstance().rhiDevice();
    if (!device || m_points.empty() || m_indices.empty()) {
        return false;
    }

    ensureRHIResources(device);

    // 展开数据重烘条件：数据/宽度变化，或视口尺寸变化（像素→NDC 换算烘在
    // 顶点属性里，随视口刷新；仅比较尺寸，无变化时零额外开销）。
    // 注意顺序：上传在句柄校验之前——首次绘制时 buffer 正是在此惰性创建
    refreshRhiViewport();
    const bool needBake = m_rhiDirty
        || m_rhiBakedViewportW != m_rhiViewportW
        || m_rhiBakedViewportH != m_rhiViewportH;
    if (needBake) {
        rebuildRhiExpandData();
        uploadRhiExpandData(device);
        m_rhiBakedViewportW = m_rhiViewportW;
        m_rhiBakedViewportH = m_rhiViewportH;
        m_rhiDirty = false;
    }
    if (!m_rhiPipeline.valid() || !m_rhiVB.valid() || !m_rhiIB.valid()) {
        return false;
    }

    auto* cmd = device->commandList();
    // 注：Line 在 NormalPass / RhiScene 的 render pass 内部被调用（GL 下状态机可嵌套；
    // Vulkan 后端时这里变为向当前 pass 录制，不再单独 beginRenderPass）
    cmd->bindPipeline(m_rhiPipeline);

    glm::mat4 vp = Matrix::toMatrix(mtx);
    cmd->setUniform("uMatrixMVP", rhi::UniformValue::fromMat4(glm::value_ptr(vp)));
    cmd->setUniform("uColor", rhi::UniformValue::fromVec3(&m_color.x));

    cmd->bindVertexBuffer(m_rhiVB);
    cmd->bindIndexBuffer(m_rhiIB);
    cmd->drawIndexed(static_cast<uint32_t>(m_rhiIndices.size()));
    return true;
}

void Line::ensureRHIResources(rhi::IRHIDevice* device) {
    // 管线：D1 宽线着色器（vert_line_rhi / frag_line_rhi，GL/VK 各自变体同名）。
    // 展开后为三角形图元；线是细长条带，任何绕序都可能出现 → 关闭裁剪
    if (!m_rhiPipeline.valid()) {
        rhi::GraphicsPipelineDesc desc;
        desc.shaders = {
            { rhi::ShaderStage::Vertex,   "vert_line_rhi" },
            { rhi::ShaderStage::Fragment, "frag_line_rhi" },
        };
        desc.vertexLayout.stride = sizeof(ExpandVertex);   // 48B：posA3+posB3+meta4+screen2
        desc.vertexLayout.attributes = {
            { 0, rhi::VertexFormat::Float3,  0 },
            { 1, rhi::VertexFormat::Float3, 12 },
            { 2, rhi::VertexFormat::Float4, 24 },
            { 3, rhi::VertexFormat::Float2, 40 },
        };
        desc.primitive = rhi::PrimitiveType::Triangles;
        desc.cull      = rhi::CullMode::None;
        desc.depth     = { true, true };
        desc.blend     = { false };
        m_rhiPipeline = device->createPipeline(desc);
    }
}

void Line::calculate() {
    // 注意：这里不能在结尾 glBindVertexArray(0) 解绑——
    // 核心配置下 glDrawElements 配 VAO 0 是非法调用（静默不绘制），
    // 修复前坐标轴从未真正显示过（该 bug 由 RHI A/B 对比测试发现）。
    glBindVertexArray(_VAO);

    // bind
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, _EBO);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(GL_UNSIGNED_INT) * m_indices.size(), m_indices.data(), GL_STATIC_DRAW);


    glBindBuffer(GL_ARRAY_BUFFER, _VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(glm::vec3) * m_points.size(), m_points.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
}
