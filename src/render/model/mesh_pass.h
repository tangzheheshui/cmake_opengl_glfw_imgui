//
//  pass.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/22.
//

#ifndef pass_hpp
#define pass_hpp

#include <vector>
#include <map>
#include "rhi/RhiTypes.h"
#include "glm/glm.hpp"
#include "camera/camera.h"
#include "model_data.h"
#include "math/matrix.h"

namespace rhi { class IRHIDevice; }
namespace rhi { class IRHICommandList; }

class MeshPass {
public:
    MeshPass(std::shared_ptr<MeshData> meshData, std::shared_ptr<Material> matData);
    // 绘制入口（GL/VK 同一 RHI 路径）
    bool Draw(const std::vector<Matrix> &matModel, const Matrix &mtx, uint32_t flags, int numViewpoit, float opacity = 1.0f);
    // D3：opacity < 1 走 blend 管线变体（depthWrite 关 + srcAlpha 混合）
    bool DrawRHI(const std::vector<Matrix> &matModel, const Matrix &mtx, uint32_t flags, float opacity = 1.0f);
    // D2: RHI 拾取绘制（pick 管线 + ID 色直出；调用方管理 RT/pass）
    bool DrawPickRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle pickPipeline,
                     const Matrix& viewProj, const std::vector<Matrix>& matModel,
                     uint16_t pickID);
    // V3-18：阴影 Pass 绘制（深度专用管线 + 光空间矩阵）
    bool DrawShadowRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle shadowPipeline,
                       const Matrix& lightVP, const std::vector<Matrix>& matModel);
protected:
    void ensureRHIResources(rhi::IRHIDevice* device);   // 惰性创建 RHI 资源
public:
    std::shared_ptr<Material> m_material;
    std::shared_ptr<MeshData> m_mesh_data;
    bool m_textured = false;   // 材质带纹理 → 贴图着色器；否则纯色着色器
    std::shared_ptr<std::vector<glm::mat4>> m_matBone;

    // ---- RHI 资源（M4.2 迁移）----
    rhi::BufferHandle m_rhiVB;
    rhi::BufferHandle m_rhiInstanceVB;
    rhi::BufferHandle m_rhiIB;
    rhi::PipelineHandle m_rhiPipeline;
    rhi::PipelineHandle m_rhiPipelineBlend;   // D3：透明变体（blend+depthWrite off，惰性建）
    rhi::PipelineHandle m_rhiPipelineWire;    // D5：线框变体（solid 着色器 + Line 模式，惰性建）
    rhi::GraphicsPipelineDesc _lastPipelineDesc;   // D3：blend 变体从 opaque desc 复制改状态
    rhi::UniformBlockHandle m_rhiUniformBlock;
    // E4：LOD——顶点聚簇简化网格（惰性构建，首次需要时生成）
    rhi::BufferHandle m_rhiVB1;
    rhi::BufferHandle m_rhiIB1;
    uint32_t          m_lod0Tris = 0;
    uint32_t          m_lod1Tris = 0;
    bool              m_lodBuilt = false;
    bool ensureLodGeometry(rhi::IRHIDevice* device);
    bool m_rhiBuilt = false;
    std::map<std::string, rhi::TextureHandle> m_rhiTexCache;   // 材质纹理缓存（文件名 → RHI 纹理）
};

#endif /* pass_hpp */
