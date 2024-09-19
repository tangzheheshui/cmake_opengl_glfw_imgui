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
#include "camera.h"

const GLuint SHADOW_WIDTH = 1024, SHADOW_HEIGHT = 1024;
const unsigned int SCR_WIDTH = 800;
const unsigned int SCR_HEIGHT = 600;

Scene& Scene::getScene() {
    static Scene instance;
    return instance;
}

Scene::Scene() {
    // 创建阴影贴图
    glGenFramebuffers(1, &_depthMapFBO);
    
    // 创建alpha链表纹理
    creatBlendTexture();
    
    // 加载贴图
    createObjs();

    // ���
    m_camera = std::make_shared<CCamera>();
    m_camera->setTarget({0, 0, 0});
    m_camera->setPos({0, 40, 40});
    m_camera->setUp({0, 1, 0});
    m_camera->setFov(60);
    m_camera->setNearfar({0.1, 500});
    m_camera->setAspect(SCR_WIDTH / SCR_HEIGHT);
    m_camera->updateViewMatrix();
    m_camera->updateProjMatrix();
}

std::vector<std::shared_ptr<ImageRectangle>> Scene::createGlass() {
    std::vector<std::shared_ptr<ImageRectangle>> vec_obj;
    vec_obj.push_back(std::make_shared<ImageRectangle>());
    vec_obj.push_back(std::make_shared<ImageRectangle>());
    vec_obj.push_back(std::make_shared<ImageRectangle>());
    float ground_width = 5;
    
    vec_obj[0]->setImagePath("window.png", "", "");
    vec_obj[1]->setImagePath("block.png", "", "");
    vec_obj[2]->setImagePath("grass.png", "", "");
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
    objGround->setImagePath("bricks2.jpg", "bricks2_normal.jpg", "bricks2_disp.jpg");
    
    objGround->setSetp(5, 5);
    glm::vec3 p1(-ground_width, 0,  ground_width);
    glm::vec3 p2(ground_width, 0,  ground_width);
    glm::vec3 p3(ground_width, 0, -ground_width);
    glm::vec3 p4(-ground_width, 0, -ground_width);
    objGround->setPoints(p1, p2, p3, p4);
    objGround->calculate();
    
    // 背包
    std::shared_ptr<Model> objModel = std::make_shared<Model>();
    //objModel->LoadFile("/Users/liuhaifeng/personal/OpenglLearnMac/OpenglLearn/res/model/backpack/backpack.obj");
    
    objModel->setCount(3);
    objModel->setPosition(0, {0, 1, 0});
    objModel->setScale(0, 0.5);
    
    objModel->setPosition(1, {-4, 1, 0});
    objModel->setScale(1, 0.5);
    
    objModel->setPosition(2, {4, 1, 0});
    objModel->setScale(2, 0.5);
    
    // 鸭子
    std::shared_ptr<Model> objDuck = std::make_shared<Model>();
    objDuck->LoadFile("E:/project/cmake_opengl_glfw_imgui/res/model/duck.dae");
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
    objLight->LoadFile("E:/project/cmake_opengl_glfw_imgui/res/model/OBJ/box.obj");
    
    objLight->setCount(1);
    objLight->setLightOpen(false);
    auto lightPos = Light::GlobalLight().position;
    objLight->setPosition(0, {lightPos.x, lightPos.y, lightPos.z});
    objLight->setScale(0, 0.5);
    
    // 天空�?
    std::shared_ptr<Sky> objSky = std::make_shared<Sky>();
    objSky->setCubeImage({"skybox/right.jpg", "skybox/left.jpg", "skybox/top.jpg", "skybox/bottom.jpg", "skybox/front.jpg", "skybox/back.jpg"});
    // debug deep
//    std::shared_ptr<Image> objImage = std::make_shared<Image>();
//    objImage->setTextureID(GetShadowTexture());
//    objImage->setShaderType(ShaderType::Debug_DeepTexture);
    
    // push 
    m_vec_drawobj.push_back(objGround);
    //m_vec_drawobj.push_back(objDuck);
    //m_vec_drawobj.push_back(objModel);
    //m_vec_drawobj.push_back(objLight);
    m_vec_drawobj.push_back(line_x);
    m_vec_drawobj.push_back(line_y);
    m_vec_drawobj.push_back(line_z);
    //m_vec_drawobj.push_back(objImage);
    //auto glass = createGlass();
    //m_vec_drawobj_blend.insert(m_vec_drawobj_blend.end(), glass.begin(), glass.end());
    
    // 最后绘制天空盒
    m_vec_drawobj.push_back(objSky);
}

void Scene::update() {
    for (auto obj : m_vec_drawobj) {
        obj->update();
    }
}

void Scene::draw() {
    glViewport(0, 0, SCR_WIDTH*2, SCR_HEIGHT*2);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glClearColor(0.2f, 0.3f, 0.3f, 1.0f);
    glDisable(GL_BLEND);
    
    // set4Viewport();
    for (auto obj : m_vec_drawobj) {
        obj->draw();
    }
    
    // 半透明渲染到贴图上
    for (auto obj : m_vec_drawobj_blend) {
        obj->draw();
    }
    
    // 渲染半透明效果
    
    // 画debug�?
    auto testLine = getTestLine();
    //testLine->draw();
}

void Scene::drawShadow() {
    // attach到fbo
    glBindFramebuffer(GL_FRAMEBUFFER, _depthMapFBO);
    auto _depthTexture = TextureMng::getInstance().getTexture(STR_DEPTH_TEXTURE);
    assert(_depthTexture);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, _depthTexture, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    // bind
    glViewport(0, 0, SHADOW_WIDTH, SHADOW_HEIGHT);
    glBindFramebuffer(GL_FRAMEBUFFER, _depthMapFBO);
    glClear(GL_DEPTH_BUFFER_BIT);
    
    // draw
    for (auto obj : m_vec_drawobj) {
        obj->drawShadow();
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

void Scene::setLightUniform(Shader* shader) {
    auto light = Light::GlobalLight();
    shader->setFloat4("uLight.position", light.position.x, light.position.y, light.position.z, 1);
    shader->setFloat3("uLight.direction", light.direction.x, light.direction.y, light.direction.z);
    shader->setFloat("uLight.cosTheta", light.cosTheta);
    shader->setFloat3("uLight.ambient", light.ambient.x, light.ambient.y, light.ambient.z);
    shader->setFloat3("uLight.diffuse", light.diffuse.x, light.diffuse.y, light.diffuse.z);
    shader->setFloat3("uLight.specular", light.specular.x, light.specular.y, light.specular.z);
}

void Scene::processMouseClick(double x, double y) {
    // 计算射线
    glm::vec3 worldNear;
    Camera::screenToWorld({x, y}, worldNear);
    
    glm::vec3 cameraPos = Camera::GetCamera().getPossition();
    
    float deep = -2.f;
    for (auto obj : m_vec_drawobj) {
        if (auto modelObj = std::dynamic_pointer_cast<Model>(obj)) {
            modelObj->isClick(cameraPos, worldNear, deep);
        }
    }
}

void Scene::creatBlendTexture() {
    // 2D纹理
    GLuint _texture_blend;
    glGenTextures(1, &_texture_blend);
    glBindTexture(GL_TEXTURE_2D, _texture_blend);
    glTexImage2D(GL_TEXTURE_2D, 0, 
                 GL_R32UI,
                 SHADOW_WIDTH,
                 SHADOW_WIDTH,
                 0,
                 GL_RED_INTEGER,
                 GL_UNSIGNED_INT,
                 NULL);
    glBindTexture(GL_TEXTURE_2D, 0);
    
    // pbo
    int totalsize = SHADOW_WIDTH * SHADOW_HEIGHT * sizeof(GLuint);
    glGenBuffers(1, &_pbo_head_pointer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _pbo_head_pointer);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, totalsize, NULL, GL_STATIC_DRAW);
    GLuint* data = (GLuint*)glMapBuffer(GL_PIXEL_UNPACK_BUFFER, GL_WRITE_ONLY);
    memset(data, 0xfFF, totalsize);
    glUnmapBuffer(GL_PIXEL_UNPACK_BUFFER);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
//    
//    // 原子计数�?
    GLuint _atomic_counter;
    glGenBuffers(1, &_atomic_counter);
    glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, _atomic_counter);
    glBufferData(GL_ATOMIC_COUNTER_BUFFER, sizeof(GLuint), 0, GL_DYNAMIC_COPY);
    
    // 创建一个较大的buffer，存储每个像素的颜色深度信息
    GLuint _fragment_alpha_buffer;
    glGenBuffers(1, &_fragment_alpha_buffer);
    glBindBuffer(GL_TEXTURE_BUFFER, _fragment_alpha_buffer);
    glBufferData(GL_TEXTURE_BUFFER, 
                 2 * SHADOW_WIDTH * SHADOW_HEIGHT * sizeof(glm::vec4),
                 NULL,
                 GL_DYNAMIC_COPY);
}

void Scene::clearBlendTexture() {
    // 原子计数
    const GLuint zero = 0;
    glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, _atomic_counter);
    glBufferSubData(GL_ATOMIC_COUNTER_BUFFER, 0, sizeof(zero), &zero);
    
    // 清空
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, _pbo_head_pointer);
    glBindTexture(GL_TEXTURE_2D, _texture_blend);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, SHADOW_WIDTH, SHADOW_HEIGHT, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, NULL);
    
    // 读写
    glBindImageTexture(0, _texture_blend, 0, GL_FALSE, 0, GL_READ_WRITE, GL_R32UI);
}

void Scene::set4Viewport() {
    const float w = SCR_WIDTH;
    const float h = SCR_HEIGHT;
    
    glViewportIndexedf(0, 0, 0, w, h);
    glViewportIndexedf(1, w, 0, w, h);
    glViewportIndexedf(2, 0, h, w, h);
    glViewportIndexedf(3, w, h, w, h);
}
