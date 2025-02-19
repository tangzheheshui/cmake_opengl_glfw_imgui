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
#include "camera/camera.h"
#include "camera/CameraController.h"

Scene& Scene::getScene() {
    static Scene instance;
    return instance;
}

Scene::Scene() {
}

void Scene::init() {
    createObjs();
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

void Scene::createObjs() {
    // 绘制坐标�?
    std::vector<unsigned int> indices = {0, 1};
    
    float len = 15;
    glm::vec3 pZero = {0, 0, 0};
    glm::vec3 pX = {len, 0, 0};
    glm::vec3 pY = {0, len, 0};
    glm::vec3 pZ = {0, 0, len};
    // 3个轴
    std::shared_ptr<Line> line_x = std::make_shared<Line>();
    std::shared_ptr<Line> line_y = std::make_shared<Line>();
    std::shared_ptr<Line> line_z = std::make_shared<Line>();
    line_x->setData({pZero, pX}, indices);
    line_y->setData({pZero, pY}, indices);
    line_z->setData({pZero, pZ}, indices);
    
    // 颜色
    line_x->setColor({1, 0, 0});
    line_y->setColor({0, 1, 0});
    line_z->setColor({0, 0, 1});
    
    auto start = std::chrono::high_resolution_clock::now();
    // 地面
    std::shared_ptr<ImageRectangle> objGround = std::make_shared<ImageRectangle>();
    float ground_width = 10;
    objGround->setImagePath(m_root_path + "/res/textures/bricks2.jpg", m_root_path + "/res/textures/bricks2_normal.jpg", m_root_path + "/res/textures/bricks2_disp.jpg");
    
    objGround->setSetp(5, 5);
    glm::vec3 p1(-ground_width, 0,  ground_width);
    glm::vec3 p2(ground_width, 0,  ground_width);
    glm::vec3 p3(ground_width, 0, -ground_width);
    glm::vec3 p4(-ground_width, 0, -ground_width);
    objGround->setPoints(p1, p2, p3, p4);
    objGround->calculate();
    
    // 背包
    std::shared_ptr<Model> objModel = std::make_shared<Model>();
    objModel->LoadFile(m_root_path + "/res/model/duck.dae");
    
    objModel->setCount(3);
    objModel->setPosition(0, {0, 1, 0});
    objModel->setScale(0, 0.5);
    
    objModel->setPosition(1, {-4, 1, 0});
    objModel->setScale(1, 0.5);
    
    objModel->setPosition(2, {4, 1, 0});
    objModel->setScale(2, 0.5);
    
    // 鸭子
    std::shared_ptr<Model> objDuck = std::make_shared<Model>();
    objDuck->LoadFile(m_root_path + "/res/model/duck.dae");
    TaskQueue::instance().pushTask([start](){
    });
    
    //objDuck->setMultiViewportNum(2);
    objDuck->setCount(4);
    objDuck->setPosition(0, {0, 0, 2});
    objDuck->setScale(0, 0.01);
    objDuck->setPosition(1, {-2, 0, 2});
    objDuck->setScale(1, 0.01);
    objDuck->setPosition(2, {2, 0, 2});
    objDuck->setScale(2, 0.01);
    objDuck->setPosition(3, {0, 0, -2});
    objDuck->setScale(3, 0.01);
    objDuck->setRotateY(3, 90);
    
    // 光源模型
    std::shared_ptr<Model> objLight = std::make_shared<Model>();
    objLight->LoadFile(m_root_path + "/res/model/duck.dae");
    
    objLight->setCount(1);
    objLight->setLightOpen(false);
    auto lightPos = Light::GlobalLight().position;
    objLight->setPosition(0, {lightPos.x, lightPos.y, lightPos.z});
    objLight->setScale(0, 0.5);
    
    // 天空�?
    std::shared_ptr<Sky> objSky = std::make_shared<Sky>();
    objSky->setCubeImage({"/skybox/right.jpg", "/skybox/left.jpg", "/skybox/top.jpg", "/skybox/bottom.jpg", "/skybox/front.jpg", "/skybox/back.jpg"});
    // debug deep
//    std::shared_ptr<Image> objImage = std::make_shared<Image>();
//    objImage->setTextureID(GetShadowTexture());
//    objImage->setShaderType(ShaderType::Debug_DeepTexture);
    
    // push 
    m_vec_drawobj.push_back(objGround);
    m_vec_drawobj.push_back(objDuck);
    //m_vec_drawobj.push_back(objModel);
    m_vec_drawobj.push_back(objLight);
    m_vec_drawobj.push_back(line_x);
    m_vec_drawobj.push_back(line_y);
    m_vec_drawobj.push_back(line_z);
    //m_vec_drawobj.push_back(objImage);
    auto glass = createGlass();
    m_vec_drawobj_blend.insert(m_vec_drawobj_blend.end(), glass.begin(), glass.end());
    
    // 最后绘制天空盒
    //m_vec_drawobj.push_back(objSky);
    SetSkyBox(objSky);
    
    // camera
    mCameraActive = std::make_shared<Camera>();
    mCameraActive->setPosition({ 0,0,40 });
    
    mCameraController = std::make_shared<CCameraController>(mCameraActive);
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
