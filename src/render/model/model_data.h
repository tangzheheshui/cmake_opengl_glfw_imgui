//
//  model_data.h
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/23.
//

#ifndef model_data_h
#define model_data_h

#include "log/LogMacros.h"
#include <glm/detail/type_quat.hpp>
#include <glm/gtx/quaternion.hpp>
#include "image.h"

#define MAX_BONE_INFLUENCE 4

struct MeshData {
    std::vector<glm::vec3> positions;
    std::vector<glm::vec3> normals;
    std::vector<glm::vec2> coords;
    std::vector<glm::vec3> tangents;
    std::vector<glm::vec3> bitTangents;
    std::vector<glm::ivec4> boneIDs;
    std::vector<glm::vec4> weights;
    std::vector<unsigned int> indices;
    int index_material = -1;
};

struct Material {
    glm::vec4 diffuse  = glm::vec4(1.0f);   // 白（albedo 乘数），否则贴图被垃圾值染色
    glm::vec4 specular = glm::vec4(1.0f);
    glm::vec4 ambient  = glm::vec4(1.0f);
    std::vector<Texture> textures;
    float shininess = 0.f;
    float shininess_strength = 0.f;
    // PBR（B1）：金属度/粗糙度/环境光遮蔽。默认非金属、中等粗糙、无遮蔽
    float metallic  = 0.0f;
    float roughness = 0.5f;
    float ao        = 1.0f;
};

struct KeyPosition
{
    glm::vec3 position;
    float timeStamp;
};

struct KeyRotation
{
    glm::quat qua;
    float timeStamp;
};

struct KeyScale
{
    glm::vec3 scale;
    float timeStamp;
};

struct NodeAnim {
    std::string name;
    //glm::mat4 localTransform; 计算的结�?
    std::vector<KeyPosition> positions;
    std::vector<KeyRotation> rotations;
    glm::mat4 getMat4(float ratio) {
        glm::mat4 translation = getPosition(ratio);
        glm::mat4 rotation = getRotation(ratio);
        return translation * rotation;
    }
    
    glm::mat4 getPosition(float ratio) {
        int index = ratio * positions.size();
        index = (index == positions.size()) ? (index-1) : index;
        ENGINE_LOG_WARN_ONCE("NodeAnim::getPosition index out of range: %d / %zu", index, positions.size());
        return glm::translate(glm::mat4(1.0f), positions[index].position);
    }
    
    glm::mat4 getRotation(float ratio) {
        int index = ratio * rotations.size();
        index = (index == rotations.size()) ? (index-1) : index;
        auto rotation = glm::normalize(rotations[index].qua);
        return glm::toMat4(rotation);
    }
};

struct Animation {
    std::string name;
    double duration = 0;
    double ticksPerSecond = 0;
    std::vector<NodeAnim> nodeAnims;
    
    NodeAnim* findNodeAnim(const std::string &name) {
        for(auto &node : nodeAnims) {
            if (node.name == name) {
                return &node;
            }
        }
        return nullptr;
    }
};

struct Node {
    std::string name;
    glm::mat4 transformation;
    glm::mat4 matCur = glm::mat4(1);
    std::vector<std::shared_ptr<Node>> child;
};

struct BoneInfo {
    std::string nodeName;
    int index = -1;
    glm::mat4 offset;
};

struct ModelData {
    std::vector<std::shared_ptr<MeshData>> meshs; // 必须�?
    std::map<std::string, ImageBuffer> map_image;
    std::vector<std::shared_ptr<Material>> materials;
    std::vector<std::shared_ptr<Node>> nodes; // 第一个就是根节点
    Animation animation;
    std::map<std::string, BoneInfo> mapBoneInfo;
};

enum DrawOption : char {
    NONE = 0,
    DRAW_SHADOW = (1<<0),
    LIGHT_OPEN  = (1<<1),
    MULTI_VIEWPORT = (1<<2),
    DRAW_PICK = (1<<3),
    DRAW_HIGHLIGHT = (1<<4),   // D8：本对象被选中（frag 侧 uHighlight 混合）
    DRAW_HOVER     = (1<<5),   // 悬停提示（uHover 混合；选中优先级更高）
};

#endif /* model_data_h */
