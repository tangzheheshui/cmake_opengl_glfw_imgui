// camera

#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "math/matrix.h"

class CCamera {
public:
    CCamera();

    void setPos(const glm::vec3 &);
    glm::vec3 getPos() const;

    void setTarget(const glm::vec3&);
    glm::vec3 getTarget() const;

    void setUp(const glm::vec3&);
    glm::vec3 getUp() const;

    void setFov(float fov);
    float getFov() const;
    
    void setAspect(float);
    float getAspect() const;

    void setNearfar(const glm::vec2&);
    glm::vec2 getNearfar() const;

    void updateViewMatrix();

    void updateProjMatrix();

    glm::mat4 GetVPMatrix();
    glm::mat4 GetViewMatrix();
    glm::mat4 GetProjectMatrix();
    
    void screenToWorld(const glm::vec2 &screen, glm::vec3 &world);
private:
    glm::vec3 _position;
    glm::vec3 _target;
    glm::vec3 _up;
    glm::vec2 _nearfar;
    float _aspect;
    float _fov;
    glm::mat4 _viewMatrix;
    glm::mat4 _projMatrix;
};

