//
//  model.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/3.
//

#include "model.hpp"
#include "core/taskQueue.h"
#include <iostream>
#include <filesystem>
#include "core/math/math.hpp"

void Model::LoadFile(const std::string &path) {
    if (!m_filepath.empty()) {
        assert(0); // 不允许变更模型信息
        return;
    }
    // Load the model file.
    Assimp::Importer importer;
    const aiScene* scene = importer.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs);
    //import.ReadFile(path, aiProcess_Triangulate | aiProcess_FlipUVs); 
    
    // check for errors
    if(!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE || !scene->mRootNode) // if is Not Zero
    {
        std::cout << "ERROR::ASSIMP:: " << importer.GetErrorString() << std::endl;
        assert(0);
        return;
    }
    
    // retrieve the directory path of the filepath
    m_filepath = path.substr(0, path.find_last_of('/') + 1);
    
    processTexture(scene);
    
    // 解析材质
    processMaterail(scene);
    
    // process ASSIMP's root node recursively
    processNode(scene->mRootNode, scene, nullptr);
    
    // 解析动画
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
            printf("parserAni, nodeName = %s\n", nodeAni.name.c_str());
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
        // 目前只取一个动画
        m_model_data.animation = animation;
        break;
    }
}

void Model::processMaterail(const aiScene* scene) {
    if (!scene || !scene->HasMaterials()) {
        return;
    }
    for (unsigned int i = 0; i < scene->mNumMaterials; i++) {
        aiMaterial *mat = scene->mMaterials[i];
        if (!mat) {
            continue;
        }
        std::shared_ptr<Materail> tmpMat = std::make_shared<Materail>();
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
        m_model_data.materails.push_back(tmpMat);
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
    meshData->index_materail = mesh->mMaterialIndex;

    processBoneWeightForVertices(mesh, meshData);
    
    auto pMesh = std::make_shared<Mesh>(meshData, m_model_data.materails[mesh->mMaterialIndex]);
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
            texture.filepath = m_filepath + name;
        }
        m_map_tempTexture[filename] = texture;
        textures.push_back(texture);
    }
    return textures;
}

void Model::genMesh() {
    
}

bool Model::drawShadow() {
    uint32_t flags = 0;
    flags |= DrawOption::DRAW_SHADOW;
    for (auto mesh : m_mesh) {
        mesh->Draw(m_vec_modelMat, flags);
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
        printf("update ratio, m_anim_ratio = %f, cur = %d\n", m_anim_ratio, cur);
        updateNode(m_model_data.nodes.front().get(), nullptr);
    }
}

bool Model::draw() {
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);
    
    uint32_t flags = 0;
    if (getLightOpen()) {
        flags |= DrawOption::LIGHT_OPEN;
    }
    
    if (m_nMultiViewport > 1) {
        flags |= DrawOption::MULTI_VIEWPORT;
    }
    
    for (auto mesh : m_mesh) {
        mesh->setBoneMat(m_FinalBoneMatrices);
        mesh->Draw(m_vec_modelMat, flags, m_nMultiViewport);
    }
    glDisable(GL_CULL_FACE);
    return true;
}

void Model::setCount(int count) {
    m_vec_modelMat.resize(count, Matrix());
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
