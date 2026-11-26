//
//  model.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/3.
//

#include "log/LogMacros.h"
#include "model.h"
#include "RenderSystem.h"
#include "rhi/IRHIDevice.h"
#include "core/taskQueue.h"
#include "fs/FilePathUtil.h"
#include "Utf8IOSystem.h"
#include <chrono>
#include <future>
#include <iostream>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include "core/math/math.h"

// 合批前置：同文件模型解析缓存——Assimp 解析（CPU 最重的一段）每路径一次；
// 命中后 MeshData/材质/节点全部浅共享（shared_ptr），Mesh/MeshPass 对象仍
// 每模型独立（实例矩阵/骨骼状态/绘制 uniform 不受影响）。
// 注意：含骨骼动画的模型不入缓存（节点 matCur 是动画状态，浅共享会串扰）。
// E5：缓存值改为 shared_future——prefetchAsync 后台解析与主线程 LoadFile
// 经同一 future 汇合（先到先解析、后到等待）。
namespace {
struct CachedModelData {
    ModelData data;                                   // 材质/节点/动画/骨骼（浅共享）
    std::vector<std::shared_ptr<MeshData>> meshDatas; // 各 mesh 几何
    AABB localAabb;                                   // 局部包围盒（E3 剔除用，解析期算出）
};
std::mutex s_parseCacheMutex;
std::map<std::string, std::shared_future<std::shared_ptr<CachedModelData>>> s_parseFutures;

// 从一个解析完成的 Model 提取缓存内容
std::shared_ptr<CachedModelData> extractCache(const Model& parser) {
    auto cached = std::make_shared<CachedModelData>();
    cached->data = parser.getModelData();
    for (const auto& mesh : parser.getMeshes()) {
        if (auto md = mesh->meshData()) cached->meshDatas.push_back(md);
    }
    cached->localAabb = parser.getLocalAABB();
    return cached;
}

// 动画模型不能进缓存（节点 matCur 是每模型可变状态，浅共享会串扰）
bool isCachable(const CachedModelData& c) {
    return c.data.animation.nodeAnims.empty();
}
} // namespace

void Model::LoadFile(const std::string &path) {
    if (!m_filepath.empty()) {
        assert(0); // 不允许变更模型信息
        return;
    }
    m_filepath = engine::fs::pathFromUtf8(path).parent_path();

    // E5：从缓存/future 获取解析结果（prefetchAsync 预解析在此汇合——
    // 若后台解析未完成则短暂等待，已完成则立即返回）
    std::shared_ptr<CachedModelData> cached;
    std::shared_future<std::shared_ptr<CachedModelData>> pending;
    {
        std::lock_guard<std::mutex> lk(s_parseCacheMutex);
        auto it = s_parseFutures.find(path);
        if (it != s_parseFutures.end()) pending = it->second;
    }
    if (pending.valid()) {
        const auto t0 = std::chrono::steady_clock::now();
        cached = pending.get();   // 锁外等待后台解析（不阻塞其他路径的加载）
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        const bool cachable = isCachable(*cached);
        {
            std::lock_guard<std::mutex> lk(s_parseCacheMutex);
            auto it = s_parseFutures.find(path);
            if (it != s_parseFutures.end()) {
                if (cachable) {
                    // 放回 ready future（后续 LoadFile 直接命中，不重复解析）
                    std::promise<std::shared_ptr<CachedModelData>> readyPr;
                    readyPr.set_value(cached);
                    it->second = readyPr.get_future().share();
                } else {
                    s_parseFutures.erase(it);   // 动画模型：仅本次使用，后续重解析
                }
            }
        }
        ENGINE_LOG_INFO("[MODEL] parse joined: %s (waited %lld ms, %zu meshes%s)",
                        path.c_str(), static_cast<long long>(ms), cached->meshDatas.size(),
                        cachable ? "" : ", animated not cached");
    }
    if (!cached) {
        // 同步解析路径（未预取）：解析；静态模型入缓存（ready future，供并发复用）
        auto parser = std::make_shared<Model>();
        parser->loadFileInternal(path);
        cached = extractCache(*parser);
        if (isCachable(*cached)) {
            std::lock_guard<std::mutex> lk(s_parseCacheMutex);
            std::promise<std::shared_ptr<CachedModelData>> readyPr;
            readyPr.set_value(cached);
            s_parseFutures[path] = readyPr.get_future().share();
            ENGINE_LOG_INFO("[MODEL] parsed sync & cached: %s (%zu meshes)",
                            path.c_str(), cached->meshDatas.size());
        } else {
            ENGINE_LOG_INFO("[MODEL] parsed sync (animated, not cached): %s", path.c_str());
        }
    }

    m_model_data = cached->data;   // 浅拷贝（内部全部 shared_ptr/vector）
    m_aabb = cached->localAabb;    // E3 剔除依赖：缓存/异步路径同样带回包围盒
    for (const auto& md : cached->meshDatas) {
        m_mesh.push_back(std::make_shared<Mesh>(md, m_model_data.materials[md->index_material]));
    }
    m_clock = clock();
}

// E5：后台预解析（两端通用——去后端化后构造/解析全程纯 CPU，GPU 资源
// 仍在主线程绘制期经 RHI 惰性创建）。解析体 = 局部 Model 实例跑解析内核，
// 完成后经 future 交给主线程 LoadFile 汇合。
void Model::prefetchAsync(const std::string &path) {
    {
        std::lock_guard<std::mutex> lk(s_parseCacheMutex);
        if (s_parseFutures.count(path)) return;   // 已在队列
        std::promise<std::shared_ptr<CachedModelData>> pr;
        s_parseFutures[path] = pr.get_future().share();
        std::thread([path, pr = std::move(pr)]() mutable {
            const auto t0 = std::chrono::steady_clock::now();
            auto parser = std::make_shared<Model>();
            parser->loadFileInternal(path);
            auto cached = extractCache(*parser);
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            const bool cachable = isCachable(*cached);
            ENGINE_LOG_INFO("[MODEL][async] parsed on background thread: %s (%lld ms, %zu meshes%s)",
                            path.c_str(), static_cast<long long>(ms), cached->meshDatas.size(),
                            cachable ? "" : ", animated not cached");
            pr.set_value(std::move(cached));
            if (!cachable) {
                // 动画模型：主线程若尚未取走也只会拿不到 future 而同步重解析，
                // 不会误共享（保证不入缓存）
                std::lock_guard<std::mutex> lk(s_parseCacheMutex);
                s_parseFutures.erase(path);
            }
        }).detach();
        ENGINE_LOG_INFO("[MODEL][async] prefetch queued: %s", path.c_str());
    }
}

// E5：解析内核（原 LoadFile 解析体；无缓存逻辑、不动 m_clock——
// 后台线程只允许 CPU 解析，Mesh/MeshPass 构造无 GL 调用，VK 模式安全）
void Model::loadFileInternal(const std::string &path) {
    // 材质纹理的相对路径基于模型文件目录（纯路径运算，后台线程安全）
    m_filepath = engine::fs::pathFromUtf8(path).parent_path();
    Assimp::Importer importer;
    importer.SetIOHandler(new engine::Utf8IOSystem());
    const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs);

    if(!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode)
    {
        ENGINE_LOG_ERROR("ASSIMP: %s", importer.GetErrorString());
        assert(0);
        return;
    }

    processTexture(scene);
    processMaterial(scene);
    processNode(scene->mRootNode, scene, nullptr);
    processAnimation(scene);
}

void Model::processTexture(const aiScene* scene) {
    if (!scene || !scene->HasTextures()) {
        return;
    }
    for (unsigned int i = 0; i < scene->mNumTextures; i++) {
        aiTexture *texture = scene->mTextures[i];
        if (!texture) {
            continue;
        }
        //image_data.name = texture->mFilename.C_Str();
        auto name = std::string("*") + std::to_string(i);
        //std::cout << "model data, image name = " << name << std::endl;
        unsigned char* buf = (unsigned char*)texture->pcData;
        auto pData = std::make_shared<std::vector<unsigned char>>();
        pData->assign(buf, buf + texture->mWidth);
        m_model_data.map_image[name] = pData;
    }
}

void Model::processAnimation(const aiScene* scene) {
    if (!scene || !scene->HasAnimations()) {
        return;
    }
    for (unsigned int i = 0; i < scene->mNumAnimations; i++) {
        Animation animation;
        auto pAni = scene->mAnimations[i];
        animation.name = std::string(pAni->mName.data);
        animation.duration = pAni->mDuration;
        animation.ticksPerSecond = pAni->mTicksPerSecond;
        
        for(int j = 0; j < pAni->mNumChannels; j++) {
            auto channel = pAni->mChannels[j];
            NodeAnim nodeAni;
            nodeAni.name = channel->mNodeName.C_Str();
            ENGINE_LOG_INFO("parserAni, nodeName = %s", nodeAni.name.c_str());
            // position
            for (int indexPos = 0; indexPos < channel->mNumPositionKeys; indexPos++) {
                KeyPosition key;
                key.timeStamp = channel->mPositionKeys[indexPos].mTime;
                key.position = AssimpGLMHelpers::GetGLMVec(channel->mPositionKeys[indexPos].mValue);
                nodeAni.positions.push_back(key);
            }
            
            for (int indexPos = 0; indexPos < channel->mNumRotationKeys; indexPos++) {
                KeyRotation key;
                key.timeStamp = channel->mRotationKeys[indexPos].mTime;
                key.qua = AssimpGLMHelpers::GetGLMQuat(channel->mRotationKeys[indexPos].mValue);
                nodeAni.rotations.push_back(key);
            }
            
            animation.nodeAnims.push_back(nodeAni);
        }
        // 目前只取一个动�?
        m_model_data.animation = animation;
        break;
    }
}

void Model::processMaterial(const aiScene* scene) {
    if (!scene || !scene->HasMaterials()) {
        return;
    }
    for (unsigned int i = 0; i < scene->mNumMaterials; i++) {
        aiMaterial *mat = scene->mMaterials[i];
        if (!mat) {
            continue;
        }
        std::shared_ptr<Material> tmpMat = std::make_shared<Material>();
        aiColor4D diffuse;
        aiColor4D specular;
        aiColor4D ambient;
        float shininess;
        float shininess_strength;
        
        if(AI_SUCCESS == aiGetMaterialColor(mat, AI_MATKEY_COLOR_DIFFUSE, &diffuse)) {
            tmpMat->diffuse = {diffuse.r, diffuse.g, diffuse.b, diffuse.a};
        }
           
        if(AI_SUCCESS == aiGetMaterialColor(mat, AI_MATKEY_COLOR_AMBIENT, &ambient)) {
            // 防止模型导出时，丢失ambient
            aiColor4D color(0.0, 0.0, 0.0, ambient.a);
            if (ambient == color) {
                tmpMat->ambient = tmpMat->diffuse;
            } 
            else {
                tmpMat->ambient = {ambient.r, ambient.g, ambient.b, ambient.a};
            }
        }
        
        if(AI_SUCCESS == aiGetMaterialColor(mat, AI_MATKEY_COLOR_SPECULAR, &specular)) {
            tmpMat->specular = {specular.r, specular.g, specular.b, specular.a};
        }
        
        if (AI_SUCCESS == aiGetMaterialFloat(mat, AI_MATKEY_SHININESS, &shininess)) {
            tmpMat->shininess = shininess;
        }
        
        if (AI_SUCCESS == aiGetMaterialFloat(mat, AI_MATKEY_SHININESS_STRENGTH, &shininess_strength)) {
            tmpMat->shininess_strength = shininess_strength;
        }
        
        // 1. diffuse maps
        std::vector<Texture> diffuseMaps = loadMaterialTextures(mat, aiTextureType_DIFFUSE, "texture_diffuse");
        tmpMat->textures.insert(tmpMat->textures.end(), diffuseMaps.begin(), diffuseMaps.end());
        // 2. specular maps
        std::vector<Texture> specularMaps = loadMaterialTextures(mat, aiTextureType_SPECULAR, "texture_specular");
        tmpMat->textures.insert(tmpMat->textures.end(), specularMaps.begin(), specularMaps.end());
        // 3. normal maps
        std::vector<Texture> normalMaps = loadMaterialTextures(mat, aiTextureType_HEIGHT, "texture_normal");
        tmpMat->textures.insert(tmpMat->textures.end(), normalMaps.begin(), normalMaps.end());
        // 4. height maps
        std::vector<Texture> heightMaps = loadMaterialTextures(mat, aiTextureType_AMBIENT, "texture_height");
        tmpMat->textures.insert(tmpMat->textures.end(), heightMaps.begin(), heightMaps.end());
        m_model_data.materials.push_back(tmpMat);
    }
}

void Model::processNode(aiNode* node, const aiScene* scene, std::shared_ptr<Node> nodeParent) {
    for(unsigned int i = 0; i < node->mNumMeshes; i++)
    {
        aiMesh* mesh = scene->mMeshes[node->mMeshes[i]];
        m_mesh.push_back(processMesh(mesh, scene));
    }
    
    if (!nodeParent) {
        nodeParent = std::make_shared<Node>();
    }
    nodeParent->name = node->mName.data;
    // printf("parserNode, nodeName = %s\n", nodeParent->name.c_str());
    nodeParent->transformation = AssimpGLMHelpers::ConvertMatrixToGLMFormat(node->mTransformation);
    nodeParent->child.reserve(node->mNumChildren);
    for(unsigned int i = 0; i < node->mNumChildren; i++)
    {
        nodeParent->child.push_back(std::make_shared<Node>());
    }
    
    m_model_data.nodes.push_back(nodeParent);
    for(unsigned int i = 0; i < node->mNumChildren; i++)
    {
        processNode(node->mChildren[i], scene, nodeParent->child.at(i));
    }
}

void Model::processBoneWeightForVertices(aiMesh *mesh, std::shared_ptr<MeshData> meshData) {
    if (!mesh) {
        return;
    }
    
    meshData->boneIDs.resize(meshData->positions.size(), glm::vec4(-1));
    meshData->weights.resize(meshData->positions.size());
    
    for (int boneIndex = 0; boneIndex < mesh->mNumBones; ++boneIndex)
    {
        int boneID = -1;
        std::string boneName = mesh->mBones[boneIndex]->mName.C_Str();
        if (m_model_data.mapBoneInfo.find(boneName) == m_model_data.mapBoneInfo.end())
        {
            BoneInfo newBoneInfo;
            newBoneInfo.index = m_BoneCounter;
            newBoneInfo.offset = AssimpGLMHelpers::ConvertMatrixToGLMFormat(
                                                                            mesh->mBones[boneIndex]->mOffsetMatrix);
            m_model_data.mapBoneInfo[boneName] = newBoneInfo;
            boneID = m_BoneCounter;
            m_BoneCounter++;
        }
        else
        {
            boneID = m_model_data.mapBoneInfo[boneName].index;
        }
        
        assert(boneID != -1);
        
        // printf("parser boneID, boneID = %d\n", boneID);
        
        int boneIDNew = boneID;
        auto weights = mesh->mBones[boneIndex]->mWeights;
        int numWeights = mesh->mBones[boneIndex]->mNumWeights;
        for (int weightIndex = 0; weightIndex < numWeights; ++weightIndex)
        {
            int vertexId = weights[weightIndex].mVertexId;
            float weight = weights[weightIndex].mWeight;
            assert(vertexId <= meshData->positions.size());
            glm::ivec4 &curBoneID = meshData->boneIDs[vertexId];
            glm::vec4 &curWeitht = meshData->weights[vertexId];
            // printf("parser vertex, vertexId = %d, weight = %f\n", vertexId, weight);
            if (curBoneID.x == -1) {
                curBoneID.x = boneIDNew;
                curWeitht.x = weight;
            } else if (curBoneID.y == -1) {
                curBoneID.y = boneIDNew;
                curWeitht.y = weight;
            } else if (curBoneID.z == -1) {
                curBoneID.z = boneIDNew;
                curWeitht.z = weight;
            } else if (curBoneID.w == -1) {
                curBoneID.w = boneIDNew;
                curWeitht.w = weight;
            }
        }
    }
}

std::shared_ptr<Mesh> Model::processMesh(aiMesh *mesh, const aiScene *scene) {
    std::shared_ptr<MeshData> meshData = std::make_shared<MeshData>();
    int numVertices = mesh->mNumVertices;
    meshData->positions.reserve(numVertices);
    meshData->normals.reserve(numVertices);
    meshData->coords.reserve(numVertices);
  
    for(unsigned int i = 0; i < mesh->mNumVertices; i++) {
        // positions
        float x = mesh->mVertices[i].x;
        float y = mesh->mVertices[i].y;
        float z = mesh->mVertices[i].z;
        
        m_aabb.setMinX(std::min(x, m_aabb.getMinX()));
        m_aabb.setMinY(std::min(y, m_aabb.getMinY()));
        m_aabb.setMinZ(std::min(z, m_aabb.getMinZ()));
        m_aabb.setMaxX(std::max(x, m_aabb.getMaxX()));
        m_aabb.setMaxY(std::max(y, m_aabb.getMaxY()));
        m_aabb.setMaxZ(std::max(z, m_aabb.getMaxZ()));
        
        meshData->positions.push_back(AssimpGLMHelpers::GetGLMVec(mesh->mVertices[i]));
        
        // normals
        if (mesh->HasNormals())
        {
            meshData->normals.push_back(AssimpGLMHelpers::GetGLMVec(mesh->mNormals[i]));
        }
        
        // texture coordinates
        if(mesh->mTextureCoords[0]) // does the mesh contain texture coordinates?
        {
            x = mesh->mTextureCoords[0][i].x; 
            y = mesh->mTextureCoords[0][i].y;
            meshData->coords.push_back({x, y});
            
            // tangent
            if (mesh->mTangents) {
                x = mesh->mTangents[i].x;
                y = mesh->mTangents[i].y;
                z = mesh->mTangents[i].z;
                meshData->tangents.push_back({x, y, z});
            }
            
            // bitangent
            if (mesh->mBitangents) {
                x = mesh->mBitangents[i].x;
                y = mesh->mBitangents[i].y;
                z = mesh->mBitangents[i].z;
                meshData->bitTangents.push_back({x, y, z});
            }
        }
    }
    // now wak through each of the mesh's faces (a face is a mesh its triangle) and retrieve the corresponding vertex indices.
    for(unsigned int i = 0; i < mesh->mNumFaces; i++)
    {
        aiFace face = mesh->mFaces[i];
        // retrieve all indices of the face and store them in the indices vector
        for(unsigned int j = 0; j < face.mNumIndices; j++)
            meshData->indices.push_back(face.mIndices[j]);        
    }
    
    // 材质索引
    meshData->index_material = mesh->mMaterialIndex;

    processBoneWeightForVertices(mesh, meshData);
    
    auto pMesh = std::make_shared<Mesh>(meshData, m_model_data.materials[mesh->mMaterialIndex]);
    return pMesh;
}

std::vector<Texture> Model::loadMaterialTextures(aiMaterial *mat, aiTextureType type, std::string typeName) {
    std::vector<Texture> textures;
    for(unsigned int i = 0; i < mat->GetTextureCount(type); i++)
    {
        aiString str;
        mat->GetTexture(type, i, &str);
        std::string filename = str.C_Str();
        
        auto iterCache = m_map_tempTexture.find(filename);
        if (iterCache != m_map_tempTexture.end()) {
            textures.push_back(iterCache->second);
            continue;
        }
        
        Texture texture;
        texture.name = typeName + std::to_string(i);
        
        //std::cout << "model materail tex name = " << filename << ", typeName = " << texture.name << std::endl;
        auto iter = m_model_data.map_image.find(filename);
        if (iter != m_model_data.map_image.end()) { 
            // 从内存中获取
            texture.data = iter->second;
        }
        else {
            size_t pos = filename.find_last_of('\\');
            if (pos == std::string::npos) {
                pos = filename.find_last_of('/');
            }
            auto name = filename.substr(pos+1, filename.length() - pos);
            texture.filepath = engine::fs::utf8FromPath(m_filepath / name);
        }
        m_map_tempTexture[filename] = texture;
        textures.push_back(texture);
    }
    return textures;
}

void Model::genMesh() {
    
}

// D3：透明排序深度键 = 第一个实例的世界平移（Matrix 列主序，平移在 [12..14]）
glm::vec3 Model::worldCenter() const {
    if (m_vec_modelMat.empty()) return glm::vec3(0.0f);
    const float* m = m_vec_modelMat[0].buffer();
    return glm::vec3(m[12], m[13], m[14]);
}

// E3：世界空间 AABB——局部盒 8 角经每个实例矩阵变换后取并集。
// 实例数多时开销 O(8×N)（纯 CPU 乘加，剔除收益远大于此）。
void Model::getWorldAABB(float& minX, float& minY, float& minZ,
                         float& maxX, float& maxY, float& maxZ) const {
    minX = minY = minZ = 1e30f;
    maxX = maxY = maxZ = -1e30f;
    const float lo[3] = { m_aabb.getMinX(), m_aabb.getMinY(), m_aabb.getMinZ() };
    const float hi[3] = { m_aabb.getMaxX(), m_aabb.getMaxY(), m_aabb.getMaxZ() };
    for (const auto& mat : m_vec_modelMat) {
        const float* m = mat.buffer();
        for (int corner = 0; corner < 8; ++corner) {
            const float lx = (corner & 1) ? hi[0] : lo[0];
            const float ly = (corner & 2) ? hi[1] : lo[1];
            const float lz = (corner & 4) ? hi[2] : lo[2];
            // 列主序：世界 = M * 局部；仿射假设（模型矩阵无透视）
            const float wx = m[0] * lx + m[4] * ly + m[8] * lz + m[12];
            const float wy = m[1] * lx + m[5] * ly + m[9] * lz + m[13];
            const float wz = m[2] * lx + m[6] * ly + m[10] * lz + m[14];
            minX = std::min(minX, wx); maxX = std::max(maxX, wx);
            minY = std::min(minY, wy); maxY = std::max(maxY, wy);
            minZ = std::min(minZ, wz); maxZ = std::max(maxZ, wz);
        }
    }
}

// V3-18：RHI 阴影绘制——所有网格经深度专用管线画进阴影贴图
bool Model::drawShadowRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle shadowPipeline,
                          const Matrix& lightVP) {
    for (auto mesh : m_mesh) {
        if (!mesh->DrawShadowRHI(cmd, shadowPipeline, lightVP, m_vec_modelMat)) {
            return false;
        }
    }
    return true;
}

// D2: RHI 拾取绘制——所有网格经 pick 管线直出本对象 ID 色
bool Model::drawPickRHI(rhi::IRHICommandList* cmd, rhi::PipelineHandle pickPipeline, const Matrix& viewProj) {
    Matrix mtx;
    for (auto mesh : m_mesh) {
        if (!mesh->DrawPickRHI(cmd, pickPipeline, viewProj, m_vec_modelMat, GetID())) {
            return false;
        }
    }
    return true;
}

void Model::updateNode(Node* node, Node* nodeParent) {
    if (!node) {
        return;
    }
    
    if (nodeParent) {
        auto nodeAni = m_model_data.animation.findNodeAnim(node->name);
        if (nodeAni) {
            node->matCur = nodeParent->matCur * nodeAni->getMat4(m_anim_ratio);
        } else {
            node->matCur = nodeParent->matCur * nodeParent->transformation;
        }
    }
    
    auto iter = m_model_data.mapBoneInfo.find(node->name);
    if (iter != m_model_data.mapBoneInfo.end()) {
        int indexBone = iter->second.index;
        glm::mat4 offset = iter->second.offset;
        m_FinalBoneMatrices->at(indexBone) = node->matCur * offset;
    }
    
    // printf("checkNode, nodeName = %s\n", node->name.c_str());
    
    for (int i = 0; i < node->child.size(); i++) {
        updateNode(node->child.at(i).get(), node);
    }
}

void Model::update() {
    if (!m_model_data.mapBoneInfo.empty()) {
        if (!m_FinalBoneMatrices) {
            m_FinalBoneMatrices = std::make_shared<std::vector<glm::mat4>>();
            m_FinalBoneMatrices->resize(100, glm::mat4(1.0));
        }
        
        clock_t det = (clock() - m_clock);
        clock_t cur = det % (5 * CLOCKS_PER_SEC);
        m_anim_ratio = cur / (5.0 * CLOCKS_PER_SEC);
        ENGINE_LOG_INFO("update ratio, m_anim_ratio = %f, cur = %zu", m_anim_ratio, cur);
        updateNode(m_model_data.nodes.front().get(), nullptr);
    }
}

bool Model::draw(rhi::IRHICommandList* cmd, const Matrix &mtx) {
    // 去后端化：裁剪/混合状态由管线描述符管理（blend 变体），无 GL 状态调用
    uint32_t flags = 0;
    if (getLightOpen()) {
        flags |= DrawOption::LIGHT_OPEN;
    }

    // D8：本对象被选中（拾取命中 → settings().selectedId）
    {
        auto& rs = RenderSystem::getInstance();
        if (rs.settings().selectedId == GetID() && rs.settings().highlightStrength > 0.0f) {
            flags |= DrawOption::DRAW_HIGHLIGHT;
        } else if (rs.settings().hoverEnabled && rs.settings().hoverId == GetID()
                   && rs.settings().hoverStrength > 0.0f) {
            flags |= DrawOption::DRAW_HOVER;   // 悬停提示（未选中时才显示）
        }
    }

    // D3：透明对象（alpha < 1）走 blend 管线变体。
    // X-ray：全部模型按设定不透明度走透明管线（幻影透视）
    float opacity = getAlpha();
    {
        auto& rs = RenderSystem::getInstance();
        if (rs.settings().xrayEnabled) {
            opacity = std::min(opacity, rs.settings().xrayOpacity);
        }
    }

    for (auto mesh : m_mesh) {
        mesh->setBoneMat(m_FinalBoneMatrices);
        mesh->Draw(cmd, m_vec_modelMat, mtx, flags, m_nMultiViewport, opacity);
    }
    return true;
}

void Model::setCount(int count) {
    m_vec_modelMat.resize(count, Matrix());
}

void Model::setMetallic(float v) {
    for (auto& m : m_mesh) if (auto mat = m->material()) mat->metallic = v;
}
void Model::setRoughness(float v) {
    for (auto& m : m_mesh) if (auto mat = m->material()) mat->roughness = v;
}
void Model::setAlbedo(const glm::vec3& c) {
    for (auto& m : m_mesh) if (auto mat = m->material()) mat->diffuse = glm::vec4(c, 1.0f);
}

void Model::setRotateX(int index, float angle) {
    if (index >= m_vec_modelMat.size() || index < 0) {
        return;
    }
    m_vec_modelMat[index].rotateX(angle);
}

void Model::setRotateY(int index, float angle) {
    if (index >= m_vec_modelMat.size() || index < 0) {
        return;
    }
    m_vec_modelMat[index].rotateY(angle);
}

void Model::setRotateZ(int index, float angle) {
    if (index >= m_vec_modelMat.size() || index < 0) {
        return;
    }
    m_vec_modelMat[index].rotateZ(angle);
}

void Model::setScale(int index, float scale) {
    if (index >= m_vec_modelMat.size() || index < 0) {
        return;
    }
    m_vec_modelMat[index].scale(scale);
}

void Model::setPosition(int index, const glm::vec3 &pos) {
    if (index >= m_vec_modelMat.size() || index < 0) {
        return;
    }
    m_vec_modelMat[index].translate(pos.x, pos.y, pos.z);
}

void Model::getDebugPoint(std::vector<glm::vec3> &vertices, std::vector<unsigned int> &indices) {    
    if (m_vec_modelMat.empty()) {
        return;
    }
    
    std::vector<unsigned int> _debugIndex = {0, 1, 1, 2, 2, 3, 3, 0,
        4, 5, 5, 6, 6, 7, 7, 4,
    0, 4, 1, 5, 2, 6, 3, 7};
    
    std::vector<glm::vec3> points;
    points.push_back(glm::vec3(m_aabb.getMinX(), m_aabb.getMinY(), m_aabb.getMinZ()));
    points.push_back(glm::vec3(m_aabb.getMaxX(), m_aabb.getMinY(), m_aabb.getMinZ()));
    points.push_back(glm::vec3(m_aabb.getMaxX(), m_aabb.getMaxY(), m_aabb.getMinZ()));
    points.push_back(glm::vec3(m_aabb.getMinX(), m_aabb.getMaxY(), m_aabb.getMinZ()));
    
    points.push_back(glm::vec3(m_aabb.getMinX(), m_aabb.getMinY(), m_aabb.getMaxZ()));
    points.push_back(glm::vec3(m_aabb.getMaxX(), m_aabb.getMinY(), m_aabb.getMaxZ()));
    points.push_back(glm::vec3(m_aabb.getMaxX(), m_aabb.getMaxY(), m_aabb.getMaxZ()));
    points.push_back(glm::vec3(m_aabb.getMinX(), m_aabb.getMaxY(), m_aabb.getMaxZ()));
    
    std::vector<glm::vec3> all_points;
    all_points.reserve(8 * m_vec_modelMat.size());
    indices.reserve(indices.size() + m_vec_modelMat.size() * _debugIndex.size());
    for (const auto &mat : m_vec_modelMat) {
        size_t indexBegin = vertices.size() + all_points.size();
        
        for (const auto &point : points) {
            all_points.push_back(Matrix::toMatrix(mat) * glm::vec4(point, 1));
        }
        
        std::transform(_debugIndex.begin(), _debugIndex.end(), std::back_inserter(indices),
                           [indexBegin](int val) { return val + indexBegin; });
    }
    vertices.insert(vertices.end(), all_points.begin(), all_points.end());
}

bool Model::isClick(const glm::vec3 &worldStart, const glm::vec3 &worldEnd, float &fDeep) {
    glm::vec3 P1(m_aabb.getMinX(), m_aabb.getMinY(), m_aabb.getMinZ());
    glm::vec3 P2(m_aabb.getMaxX(), m_aabb.getMinY(), m_aabb.getMinZ());
    glm::vec3 P3(m_aabb.getMaxX(), m_aabb.getMaxY(), m_aabb.getMinZ());
    glm::vec3 P4(m_aabb.getMinX(), m_aabb.getMaxY(), m_aabb.getMinZ());
    
    glm::vec3 P5(m_aabb.getMinX(), m_aabb.getMinY(), m_aabb.getMaxZ());
    glm::vec3 P6(m_aabb.getMaxX(), m_aabb.getMinY(), m_aabb.getMaxZ());
    glm::vec3 P7(m_aabb.getMaxX(), m_aabb.getMaxY(), m_aabb.getMaxZ());
    glm::vec3 P8(m_aabb.getMinX(), m_aabb.getMaxY(), m_aabb.getMaxZ());
   
    for (int i = 0; i < m_vec_modelMat.size(); i++) {
        auto mat = Matrix::toMatrix(m_vec_modelMat[i]);
        glm::vec4 temp1 = mat * glm::vec4(P1, 1);
        glm::vec4 temp2 = mat * glm::vec4(P7, 1);
        glm::vec3 aabbP1(temp1.x, temp1.y, temp1.z);
        aabbP1 /= temp1.w;
        glm::vec3 aabbP2(temp2.x, temp2.y, temp2.z);
        aabbP2 /= temp2.w;
        glm::vec3 dir =glm::normalize(worldEnd - worldStart);
        
        glm::vec3 interPoint = Math::intersectLinePlane(aabbP1, {0, 0, 1.f}, worldStart, dir);
        if (Math::isInsidePolygon({P1, P2, P3, P4}, interPoint)) {
            return true;
        }
        
        interPoint = Math::intersectLinePlane(aabbP2, {0, 0, 1}, worldEnd, dir);
        if (Math::isInsidePolygon({P5, P6, P7, P8}, interPoint)) {
            return true;
        }
        
        interPoint = Math::intersectLinePlane(aabbP1, {1, 0, 0}, worldEnd, dir);
        if (Math::isInsidePolygon({P1, P4, P8, P5}, interPoint)) {
            return true;
        }
        
        interPoint = Math::intersectLinePlane(aabbP2, {1, 0, 0}, worldEnd, dir);
        if (Math::isInsidePolygon({P2, P3, P7, P6}, interPoint)) {
            return true;
        }
        
        interPoint = Math::intersectLinePlane(aabbP1, {0, 1, 0}, worldEnd, dir);
        if (Math::isInsidePolygon({P1, P2, P6, P5}, interPoint)) {
            return true;
        }
        
        interPoint = Math::intersectLinePlane(aabbP2, {0, 1, 0}, worldEnd, dir);
        if (Math::isInsidePolygon({P4, P3, P7, P8}, interPoint)) {
            return true;
        }
        
    }
    return false;
}
