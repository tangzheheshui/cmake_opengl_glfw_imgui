// camera

#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "math/matrix.h"
#include <memory>
class Camera;

class CCameraController {
public:
    CCameraController(std::shared_ptr<Camera> camera) { m_camera = camera; }
    // 滚轮滚动
    void onMouseScale(double scroll);
    // 鼠标拖拽
    void onMouseDrag(double x, double y);
private:
    std::shared_ptr<Camera> m_camera;
};

