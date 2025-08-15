#include "Camera.h"
#include <cmath>

namespace scene {
Camera::Camera() 
: m_position(0, 0, 0),
m_forward(0, 0, -1),
m_up(0, 1, 0),
m_worldUp(0, 1, 0),
m_projectionType(ProjectionType::Perspective),
m_fov(3.14159f / 3.0f), // 60度
m_aspect(16.0f / 9.0f),
m_near(0.1f),
m_far(100.0f),
m_viewDirty(true),
m_projectionDirty(true) {
    updateViewMatrix();
    updateProjectionMatrix();
}

Camera::Camera(const math::vec3& position, const math::vec3& target, const math::vec3& up)
: m_position(position),
m_worldUp(up),
m_projectionType(ProjectionType::Perspective),
m_fov(3.14159f / 3.0f),
m_aspect(16.0f / 9.0f),
m_near(0.1f),
m_far(100.0f),
m_viewDirty(true),
m_projectionDirty(true) {
    lookAt(target, up);
    updateProjectionMatrix();
}

const math::mat4& Camera::getViewMatrix() const {
    if (m_viewDirty) {
        updateViewMatrix();
    }
    return m_viewMatrix;
}

const math::mat4& Camera::getProjectionMatrix() const {
    if (m_projectionDirty) {
        updateProjectionMatrix();
    }
    return m_projectionMatrix;
}

math::mat4 Camera::getViewProjectionMatrix() const {
    return getProjectionMatrix() * getViewMatrix();
}

void Camera::setPosition(const math::vec3& position) {
    m_position = position;
    m_viewDirty = true;
}

void Camera::lookAt(const math::vec3& target, const math::vec3& up) {
    m_forward = (target - m_position).normalized();
    m_right = m_forward.cross(up).normalized();
    m_up = m_right.cross(m_forward);
    m_worldUp = up;
    m_viewDirty = true;
}

void Camera::translate(const math::vec3& offset) {
    m_position += offset;
    m_viewDirty = true;
}

void Camera::rotate(float yaw, float pitch) {
    // 限制俯仰角不超过89度
    const float maxPitch = 3.14159f / 2.0f - 0.01f;
    pitch = std::fmax(-maxPitch, std::fmin(maxPitch, pitch));
    
    // 计算新的前向向量
    m_forward = math::vec3(
                           std::cos(yaw) * std::cos(pitch),
                           std::sin(pitch),
                           std::sin(yaw) * std::cos(pitch)
                           ).normalized();
    
    // 重新计算右向量和上向量
    m_right = m_forward.cross(m_worldUp).normalized();
    m_up = m_right.cross(m_forward);
    
    m_viewDirty = true;
}

void Camera::setPerspective(float fov, float aspect, float near, float far) {
    m_projectionType = ProjectionType::Perspective;
    m_fov = fov;
    m_aspect = aspect;
    m_near = near;
    m_far = far;
    m_projectionDirty = true;
}

void Camera::setOrthographic(float left, float right, float bottom, float top, 
                             float near, float far) {
    m_projectionType = ProjectionType::Orthographic;
    m_left = left;
    m_right = right;
    m_bottom = bottom;
    m_top = top;
    m_near = near;
    m_far = far;
    m_projectionDirty = true;
}

void Camera::setProjectionType(ProjectionType type) {
    m_projectionType = type;
    m_projectionDirty = true;
}

void Camera::moveForward(float distance) {
    m_position += m_forward * distance;
    m_viewDirty = true;
}

void Camera::moveRight(float distance) {
    m_position += m_right * distance;
    m_viewDirty = true;
}

void Camera::moveUp(float distance) {
    m_position += m_up * distance;
    m_viewDirty = true;
}

void Camera::updateViewMatrix() const {
    m_viewMatrix = math::mat4::lookAt(m_position, m_position + m_forward, m_up);
    m_viewDirty = false;
}

void Camera::updateProjectionMatrix() const {
    switch (m_projectionType) {
        case ProjectionType::Perspective:
            m_projectionMatrix = math::mat4::perspective(m_fov, m_aspect, m_near, m_far);
            break;
        case ProjectionType::Orthographic:
            m_projectionMatrix = math::mat4::orthographic(
                                                          m_left, m_right, m_bottom, m_top, m_near, m_far);
            break;
    }
    m_projectionDirty = false;
}
}
