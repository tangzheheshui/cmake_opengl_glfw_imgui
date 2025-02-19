//
//  camera.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/2.
//
    
#include "camera.h"

CCamera::CCamera() {

}

void CCamera::setPos(const glm::vec3& pos) {
    _position = pos;
}

glm::vec3 CCamera::getPos() const {
    return _position;
}

void CCamera::setTarget(const glm::vec3& target) {
    _target = target;
}

glm::vec3 CCamera::getTarget() const {
    return _target;
}

void CCamera::setUp(const glm::vec3& up) {
    _up = up;
}

glm::vec3 CCamera::getUp() const {
    return _up;
}

void CCamera::setFov(float fov) {
    _fov = fov;
}

float CCamera::getFov() const {
    return _fov;
}

void CCamera::setAspect(float aspect) {
    _aspect = aspect;
}

float CCamera::getAspect() const {
    return _aspect;
}

void CCamera::setNearfar(const glm::vec2& nf) {
    _nearfar = nf;
}

glm::vec2 CCamera::getNearfar() const {
    return _nearfar;
}

void CCamera::updateViewMatrix() {

}

void CCamera::updateProjMatrix() {

}

glm::mat4 CCamera::GetVPMatrix() {
    return glm::mat4();
}

glm::mat4 CCamera::GetViewMatrix() {
    return glm::mat4();
}

glm::mat4 CCamera::GetProjectMatrix() {
    return glm::mat4();
}

void CCamera::screenToWorld(const glm::vec2 &screen, glm::vec3 &world) {
    float w, h;
    glm::vec4 proj(screen.x * 2 / w - 1, screen.y * 2 / h, -1, 1);
    glm::vec4 worldNear = glm::inverse(GetVPMatrix()) * proj;
    world = {worldNear.x / worldNear.w, worldNear.y / worldNear.w, worldNear.z / worldNear.w};
}
