//
//  camera.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/2.
//
    
#include "CameraController.h"
#include "camera.h"
#include <iostream>

void CCameraController::onMouseScale(double scale) {
    float fov = m_camera->getFov();
    fov += (float)scale;
    if (fov < 3.0f)
        fov = 3.0f;
    if (fov > 90.0f)
        fov = 90.0f;
    
    m_camera->setFov(fov);
}

void CCameraController::onMouseDrag(double x, double y) {
    float yaw = m_camera->getYaw();
    yaw -= x;
    float pitch = m_camera->getPitch();
    pitch += y;

    if (pitch > 89.0f)
        pitch = 89.0f;
    if (pitch < -89.0f)
        pitch = -89.0f;

    m_camera->setPitch(pitch);
    m_camera->setYaw(yaw);
}
