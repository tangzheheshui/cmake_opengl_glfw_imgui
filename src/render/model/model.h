//
//  model.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/3.
//

#ifndef model_hpp
#define model_hpp

#include "mesh.h"
#include <map>
#include <vector>
#include <glm/glm.hpp>
#include "model_data.h"
#include "../BaseDraw.h"
#include <time.h>
#include "math/matrix.h"
#include "aabb.h"
#include "image.h"
#include <filesystem>

class AssimpGLMHelpers
{
public:

    static inline glm::mat4 ConvertMatrixToGLMFormat(const aiMatrix4x4& from)
    {
        glm::mat4 to;
        //the a,b,c,d in assimp is the row ; the 1,2,3,4 is the column
        to[0][0] = from.a1; to[1][0] = from.a2; to[2][0] = from.a3; to[3][0] = from.a4;
        to[0][1] = from.b1; to[1][1] = from.b2; to[2][1] = from.b3; to[3][1] = from.b4;
        to[0][2] = from.c1; to[1][2] = from.c2; to[2][2] = from.c3; to[3][2] = from.c4;
        to[0][3] = from.d1; to[1][3] = from.d2; to[2][3] = from.d3; to[3][3] = from.d4;
        return to;
    }

    static inline glm::vec3 GetGLMVec(const aiVector3D& vec) 
    { 
        return glm::vec3(vec.x, vec.y, vec.z); 
    }
    
    static inline glm::vec2 GetGLMVec2(const aiVector2D& vec) 
    { 
        return glm::vec2(vec.x, vec.y); 
    }

    static inline glm::quat GetGLMQuat(const aiQuaternion& pOrientation)
    {
        return glm::quat(pOrientation.w, pOrientation.x, pOrientation.y, pOrientation.z);
    }
};

class Model : public BaseDraw {
public:
    using BaseDraw::BaseDraw;
    void LoadFile(const std::string &path);
    // E5 流式加载：后台线程预解析进缓存（两端通用；解析纯 CPU，GPU 资源
    // 主线程惰性创建）。主线程随后的 LoadFile 命中缓存/等待解析完成。
    static void prefetchAsync(const std::string &path);
    virtual bool draw(rhi::IRHICommandList* cmd, const Matrix &mtx) override;
    // D3：透明排序深度键 = 第一个实例的平移（多实例取代表；CAD 单件常用）
    glm::vec3 worldCenter() const override;
    // E3：世界空间 AABB（局部盒 8 角 × 全部实例矩阵变换取并集；视锥剔除用）
    void getWorldAABB(float& minX, float& minY, float& minZ,
                      float& maxX, float& maxY, float& maxZ) const;
    // E5：解析缓存提取用（Model 为解析内核载体）
    const ModelData& getModelData() const { return m_model_data; }
    const std::vector<std::shared_ptr<Mesh>>& getMeshes() const { return m_mesh; }
    const AABB& getLocalAABB() const { return m_aabb; }
    // D2: RHI 拾取绘制（pick RT 内调用；调用方管理 pass 与回读）
    bool drawPickRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle pickPipeline, const Matrix& viewProj);
    // V3-18：阴影 Pass（深度专用管线 + 光空间矩阵）
    bool drawShadowRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle shadowPipeline, const Matrix& lightVP);
    virtual void update() override;
    virtual void getDebugPoint(std::vector<glm::vec3> &vertices, std::vector<unsigned int> &indices) override;
    virtual bool isClick(const glm::vec3 &worldStart, const glm::vec3 &worldEnd, float &fDeep) override;
    // 模型的属�?
    void setCount(int count);
    // PBR：材质参数（遍历所有 mesh 写入；供场景/面板调金属度、粗糙度）
    void setMetallic(float v);
    void setRoughness(float v);
    void setAlbedo(const glm::vec3& c);
    // PBR 贴图：type = albedo/normal/metalrough/ao；path 为空则移除该贴图（用标量兜底）
    void setPbrTexture(const std::string& type, const std::string& path);
    // PBR：读当前材质（取第一个 mesh；面板实时显示用）
    float getMetallic() const { for (auto& m : m_mesh) if (auto mat = m->material()) return mat->metallic; return 0.0f; }
    float getRoughness() const { for (auto& m : m_mesh) if (auto mat = m->material()) return mat->roughness; return 0.5f; }
    void setScale(int index, float scale);
    void setPosition(int index, const glm::vec3 &pos);
    void setRotateX(int index, float angle);
    void setRotateY(int index, float angle);
    void setRotateZ(int index, float angle);
    void setMultiViewportNum(int n) { m_nMultiViewport = n; }
private:
    // E5：解析内核（Assimp 解析体；无缓存逻辑；仅可后台跑于 VK 模式）
    void loadFileInternal(const std::string &path);
    void processNode(aiNode* node, const aiScene* scene, std::shared_ptr<Node> nodeParent);
    std::shared_ptr<Mesh> processMesh(aiMesh *mesh, const aiScene *scene);
    void processBoneWeightForVertices(aiMesh *mesh, std::shared_ptr<MeshData> meshData);
    void processTexture(const aiScene* scene);
    void processMaterial(const aiScene* scene);
    void processAnimation(const aiScene* scene);
    std::vector<Texture> loadMaterialTextures(aiMaterial *mat, aiTextureType type, std::string typeName);
    void genMesh();
    void updateNode(Node* node, Node* nodeParent);
private:
    std::vector<Matrix> m_vec_modelMat;
    std::map<std::string, Texture> m_map_tempTexture;
    std::filesystem::path m_filepath;
    std::vector<std::shared_ptr<Mesh>> m_mesh;
    ModelData m_model_data;
    std::shared_ptr<std::vector<glm::mat4>> m_FinalBoneMatrices;
    clock_t m_clock{};
    float m_anim_ratio = 0.f;
    int m_BoneCounter = 0;
    AABB m_aabb;
    int m_nMultiViewport = 0;
};

#endif /* model_hpp */
