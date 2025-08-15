#pragma once
#include "math/vector3.h"
#include "math/vector4.h"
#include "math/mat4.h"

namespace scene {
class Camera {
public:
    enum class ProjectionType {
        Perspective,
        Orthographic
    };
    
    // 构造函数
    Camera();
    Camera(const math::vec3& position, const math::vec3& target, 
           const math::vec3& up = math::vec3::up());
    
    // 获取矩阵
    const math::mat4& getViewMatrix() const;
    const math::mat4& getProjectionMatrix() const;
    math::mat4 getViewProjectionMatrix() const;
    
    // 相机控制
    void setPosition(const math::vec3& position);
    void lookAt(const math::vec3& target, const math::vec3& up = math::vec3::up());
    void translate(const math::vec3& offset);
    void rotate(float yaw, float pitch); // 欧拉角旋转
    
    // 投影设置
    void setPerspective(float fov, float aspect, float near, float far);
    void setOrthographic(float left, float right, float bottom, float top, 
                         float near, float far);
    void setProjectionType(ProjectionType type);
    
    // 获取相机参数
    math::vec3 getPosition() const { return m_position; }
    math::vec3 getForward() const { return m_forward; }
    math::vec3 getRight() const { return m_right; }
    math::vec3 getUp() const { return m_up; }
    float getNear() const { return m_near; }
    float getFar() const { return m_far; }
    
    // 相机移动
    void moveForward(float distance);
    void moveRight(float distance);
    void moveUp(float distance);
    
private:
    void updateViewMatrix() const;
    void updateProjectionMatrix() const;
    
    // 相机参数
    math::vec3 m_position;
    math::vec3 m_forward;
    math::vec3 m_up;
    math::vec3 m_right;
    math::vec3 m_worldUp;
    
    // 投影参数
    ProjectionType m_projectionType;
    union {
        struct {
            float m_fov;
            float m_aspect;
        };
        struct {
            float m_left;
            float m_cam_right;
            float m_bottom;
            float m_top;
        };
    };
    float m_near;
    float m_far;
    
    // 矩阵缓存
    mutable math::mat4 m_viewMatrix;
    mutable math::mat4 m_projectionMatrix;
    mutable bool m_viewDirty;
    mutable bool m_projectionDirty;
};
}
