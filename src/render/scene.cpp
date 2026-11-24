//
//  scene.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/25.
//

#include "scene.h"
#include "RenderSystem.h"
#include "model/model.h"
#include <algorithm>
#include <glm/glm.hpp>
#include <vector>
#include "model/model.h"
#include "object/Line.h"
#include "object/Sky.h"
#include "image.h"
#include "Light.h"

#include "camera/CameraController.h"

Scene::Scene() {
    mCameraActive = std::make_shared<Camera>();
    mCameraActive->setPosition({ 0,0,40 });
    mCameraController = std::make_shared<CCameraController>(mCameraActive);
}

void Scene::init() {
}



Matrix Scene::GetLightVPMatrix() {
    static bool initialized = false;
    if (!initialized) {
        glm::vec3 lightPos = Light::GlobalLight().position;
        float near_plane = 1.0f, far_plane = 15.f;

        Matrix lightView = Matrix::toMatrix(glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0.0000001, 1, 0.0)));
        Matrix lightProjection = Camera::perspective(degrees_to_radians(90), 1.f, near_plane, far_plane);
        _lightVPMatrix = lightProjection * lightView;
        initialized = true;
    }
    return _lightVPMatrix;
}


void Scene::AddObj(std::shared_ptr<BaseDraw> obj) {
    m_vec_drawobj.push_back(obj);
}

// D3/X-ray：透明分相谓词（X-ray 开启时模型整体视为透明），各绘制路径共用
bool Scene::isTransparentDraw(const std::shared_ptr<BaseDraw>& obj) {
    if (obj->getAlpha() < 0.999f) return true;
    if (RenderSystem::getInstance().settings().xrayEnabled) {
        return std::dynamic_pointer_cast<Model>(obj) != nullptr;
    }
    return false;
}

// D3：透明排序——不透明按加入顺序先画；透明（alpha < 1）按到相机距离
// 降序（远 → 近）后画。稳定排序保证同深度时画序确定（两端一致的前提）。
std::vector<std::shared_ptr<BaseDraw>> Scene::getSortedDrawList(const glm::vec3& camPos) const {
    std::vector<std::shared_ptr<BaseDraw>> opaque;
    std::vector<std::shared_ptr<BaseDraw>> transparent;
    opaque.reserve(m_vec_drawobj.size());
    for (const auto& obj : m_vec_drawobj) {
        if (isTransparentDraw(obj)) transparent.push_back(obj);
        else                        opaque.push_back(obj);
    }
    std::stable_sort(transparent.begin(), transparent.end(),
                     [&camPos](const std::shared_ptr<BaseDraw>& a,
                               const std::shared_ptr<BaseDraw>& b) {
                         const float da = glm::length(a->worldCenter() - camPos);
                         const float db = glm::length(b->worldCenter() - camPos);
                         return da > db;   // 远的先画
                     });
    opaque.insert(opaque.end(), transparent.begin(), transparent.end());
    return opaque;
}

