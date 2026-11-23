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

