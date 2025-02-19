//
//  camera.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/2.
//
    
#include "CameraController.h"
#include "camera_old.h"

void CCameraController::onMouseLeftDown(double x, double y) {
    mMouseLeftPressed = true;
    mPosLast = {x, y};
}

void CCameraController::onMouseLeftUp(double x, double y) {
    mMouseLeftPressed = false;
}

void CCameraController::onMouseRightDown(double x, double y) {

}

void CCameraController::onMouseRightUp(double x, double y) {
 
}

void CCameraController::onMouseMiddleScroll(double xOffset, double yOffset) {
    float fov = m_camera->getFov();
    fov += (float)yOffset;
    if (fov < 3.0f)
        fov = 3.0f;
    if (fov > 90.0f)
        fov = 90.0f;
    
    m_camera->setFov(fov);
}

void CCameraController::onMouseMiddleDown(double x, double y) {

}

void CCameraController::onMouseMove(double x, double y) {
    if (!mMouseLeftPressed) {
        return;
    }

    float xoffset = x - mPosLast.x;
    float yoffset = y - mPosLast.y;
    mPosLast = {x, y};

    float sensitivity = 0.1f;
    xoffset *= sensitivity;
    yoffset *= sensitivity;

    float yaw = m_camera->getYaw();
    yaw -= xoffset;
    float pitch = m_camera->getPitch();
    pitch += yoffset;

    if (pitch > 89.0f)
        pitch = 89.0f;
    if (pitch < -89.0f)
        pitch = -89.0f;

    m_camera->setPitch(pitch);
    m_camera->setYaw(yaw);
}
