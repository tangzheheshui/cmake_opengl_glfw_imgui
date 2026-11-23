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

#include "Line.h"
#include <glad/glad.h>
#include <glm/gtc/type_ptr.hpp>
#include "../shader.h"
#include "../RenderSystem.h"
#include "rhi/IRHIDevice.h"
#include "rhi/IRHICommandList.h"
#include "../camera/camera.h"

void Line::setData(const std::vector<glm::vec3> &points, const std::vector<unsigned int> &indices) {
    m_points = points;
    m_indices = indices;
    m_rhiDirty = true;   // RHI 路径：变化后由 ensureRHIResources 上传
}

bool Line::draw(const Matrix &mtx) {
    // ---- M2: RHI 迁移路径（开关在调试面板"迁移验证"分组）----
    auto& renderSys = RenderSystem::getInstance();
    auto* device = renderSys.rhiDevice();
    if (device && renderSys.settings().lineUseRHI) {
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

bool Line::drawRHI(const Matrix &mtx) {
    auto* device = RenderSystem::getInstance().rhiDevice();
    if (!device || m_points.empty() || m_indices.empty()) {
        return false;
    }

    ensureRHIResources(device);
    if (!m_rhiPipeline.valid() || !m_rhiVB.valid() || !m_rhiIB.valid()) {
        return false;
    }

    auto* cmd = device->commandList();
    // 注：Line 在 NormalPass 的 render pass 内部被调用（GL 下状态机可嵌套；
    // Vulkan 后端时这里变为向当前 pass 录制，不再单独 beginRenderPass）
    cmd->bindPipeline(m_rhiPipeline);

    glm::mat4 vp = Matrix::toMatrix(mtx);
    cmd->setUniform("uMatrixMVP", rhi::UniformValue::fromMat4(glm::value_ptr(vp)));
    cmd->setUniform("uColor", rhi::UniformValue::fromVec3(&m_color.x));

    cmd->bindVertexBuffer(m_rhiVB);
    cmd->bindIndexBuffer(m_rhiIB);
    cmd->drawIndexed(static_cast<uint32_t>(m_indices.size()));
    return true;
}

void Line::ensureRHIResources(rhi::IRHIDevice* device) {
    // 管线：复用现有 vert_color / frag_color（uMatrixMVP + uColor）
    if (!m_rhiPipeline.valid()) {
        rhi::GraphicsPipelineDesc desc;
        desc.shaders = {
            { rhi::ShaderStage::Vertex,   "vert_color" },
            { rhi::ShaderStage::Fragment, "frag_color" },
        };
        desc.vertexLayout.stride = sizeof(glm::vec3);
        desc.vertexLayout.attributes = {
            { 0, rhi::VertexFormat::Float3, 0 },
        };
        desc.primitive = rhi::PrimitiveType::Lines;
        desc.cull      = rhi::CullMode::None;
        desc.depth     = { true, true };
        desc.blend     = { false };
        m_rhiPipeline = device->createPipeline(desc);
    }

    // 顶点/索引流：首次惰性创建，之后仅脏标记时上传
    // 注：v1 buffer 大小固定为首次创建时尺寸，后续 setData 超出容量不扩容（见 RHI_DESIGN M2 备注）
    const uint32_t vbBytes = static_cast<uint32_t>(m_points.size() * sizeof(glm::vec3));
    const uint32_t ibBytes = static_cast<uint32_t>(m_indices.size() * sizeof(unsigned int));
    if (!m_rhiVB.valid()) {
        rhi::BufferDesc vbDesc;
        vbDesc.size = vbBytes;
        vbDesc.usage = rhi::BUF_VERTEX;
        vbDesc.initialData = m_points.data();
        m_rhiVB = device->createBuffer(vbDesc);

        rhi::BufferDesc ibDesc;
        ibDesc.size = ibBytes;
        ibDesc.usage = rhi::BUF_INDEX;
        ibDesc.initialData = m_indices.data();
        m_rhiIB = device->createBuffer(ibDesc);
        m_rhiDirty = false;
    } else if (m_rhiDirty) {
        device->updateBuffer(m_rhiVB, m_points.data(), vbBytes);
        device->updateBuffer(m_rhiIB, m_indices.data(), ibBytes);
        m_rhiDirty = false;
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
