//
//  scene.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/25.
//

#include "scene.h"
#include <glm/glm.hpp>
#include <vector>
#include "model/model.h"
#include "object/ImageRectangle.h"
#include "object/Line.h"
#include "object/Sky.h"
#include "image.h"
#include "Light.h"
#include "core/taskQueue.h"
//#include "camera/camera.h"
#include "camera/CameraController.h"
#include "math/vector2.h"
#include "math/vector3.h"
#include "math/vector4.h"
#include "math/mat4.h"
#include "camera.h"

Scene& Scene::getScene() {
    static Scene instance;
    return instance;
}

Scene::Scene() {
    mCameraActive = std::make_shared<Camera>();
    mCameraActive->setPosition({ 0,0,40 });
    mCameraController = std::make_shared<CCameraController>(mCameraActive);
}

void Scene::init() {
    math::vec2 vec2;
    math::vec3 vec3;
    math::vec4 vec4;
    math::mat4 mat;
    scene::Camera camera;
    
}

std::vector<std::shared_ptr<ImageRectangle>> Scene::createGlass() {
    std::vector<std::shared_ptr<ImageRectangle>> vec_obj;
    vec_obj.push_back(std::make_shared<ImageRectangle>());
    vec_obj.push_back(std::make_shared<ImageRectangle>());
    vec_obj.push_back(std::make_shared<ImageRectangle>());
    float ground_width = 5;
    
    vec_obj[0]->setImagePath(m_root_path + "/res/textures/window.png", "", "");
    vec_obj[1]->setImagePath(m_root_path + "/res/textures/block.png", "", "");
    vec_obj[2]->setImagePath(m_root_path + "/res/textures/grass.png", "", "");
    TaskQueue::instance().pushTask([vec_obj, ground_width](){
        for (int i = 0; i < vec_obj.size(); i++) {
            auto obj = std::dynamic_pointer_cast<ImageRectangle>(vec_obj[i]);
            
            obj->setSetp(1, 1);
            obj->setAlpha(0.5);
            float step = i * 2;
            glm::vec3 p1(0, 0, -step);
            glm::vec3 p2(ground_width, 0, -step);
            glm::vec3 p3(ground_width, ground_width, -step);
            glm::vec3 p4(0, ground_width, -step);
            obj->setPoints(p1, p2, p3, p4);
            obj->calculate();
        }
    });
    
    return vec_obj;
}


Matrix Scene::GetLightVPMatrix() {
    if (_lightVPMatrix) {
        return *_lightVPMatrix;
    }
    Matrix lightProjection, lightView;
    glm::vec3 lightPos = Light::GlobalLight().position;
    float near_plane = 1.0f, far_plane = 15.f;
    
    lightView = Matrix::toMatrix(glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0.0000001, 1, 0.0))) ;
    lightProjection = Camera::perspective(degrees_to_radians(90), 1.f, near_plane, far_plane);
    if (!_lightVPMatrix) {
        _lightVPMatrix = new Matrix;
        *_lightVPMatrix = lightProjection * lightView;
    }
    return *_lightVPMatrix;
}

std::shared_ptr<Line> Scene::getTestLine() {
    static std::shared_ptr<Line> lineObj = std::make_shared<Line>();
    glm::mat4 matVP = Matrix::toMatrix(GetLightVPMatrix());
    matVP = glm::inverse(matVP);
    std::vector<glm::vec3> frustumVertices;
    
    // 近裁剪面的四个顶�?
    frustumVertices.push_back(glm::vec3(-1, -1, -1));  // 左下
    frustumVertices.push_back(glm::vec3(1, -1, -1));   // 右下
    frustumVertices.push_back(glm::vec3(1, 1, -1));    // 右上
    frustumVertices.push_back(glm::vec3(-1, 1, -1));   // 左上
    
    // 远裁剪面的四个顶�?
    frustumVertices.push_back(glm::vec3(-1, -1, 1));   // 左下
    frustumVertices.push_back(glm::vec3(1, -1, 1));    // 右下
    frustumVertices.push_back(glm::vec3(1, 1, 1));     // 右上
    frustumVertices.push_back(glm::vec3(-1, 1, 1));    // 左上
    
    // 将顶点从 NDC 空间转换到世界空�?
    for (glm::vec3& vertex : frustumVertices) {
        glm::vec4 worldVertex = matVP * glm::vec4(vertex, 1.0f);
        vertex = glm::vec3(worldVertex) / worldVertex.w;
    }
    
    glm::vec3 lightPos = Light::GlobalLight().position;
    frustumVertices.push_back(lightPos);
    std::vector<unsigned int> indexs = {0, 1, 1, 2, 2, 3, 3, 0, 
        4, 5, 5, 6, 6, 7, 7, 4,
        0, 4, 1, 5, 2, 6, 3, 7,
    0, 8, 1, 8, 2, 8, 3, 8};
    
    // 取出m_vec_drawobj中的
    for (auto obj : m_vec_drawobj) {
        obj->getDebugPoint(frustumVertices, indexs);
    }
    
    lineObj->setData(frustumVertices, indexs);
    lineObj->setColor({0, 1, 1});
    return lineObj;
}

void Scene::AddObj(std::shared_ptr<BaseDraw> obj) {
    m_vec_drawobj.push_back(obj);
}

