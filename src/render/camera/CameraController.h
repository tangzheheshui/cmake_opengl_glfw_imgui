// camera

#pragma once

#include "glm/glm.hpp"
#include "glm/gtc/matrix_transform.hpp"
#include "math/matrix.h"
#include <memory>
class Camera;

struct Point2D {
    double x{};
    double y{};
};

class CCameraController {
public:
    CCameraController(std::shared_ptr<Camera> camera) { m_camera = camera; }

    // 左键down
    void onMouseLeftDown(double x, double y);
    // 左键up
    void onMouseLeftUp(double x, double y);
    // 右键down
    void onMouseRightDown(double x, double y);
    // 右键up
    void onMouseRightUp(double x, double y);
    // 滚轮滚动
    void onMouseMiddleScroll(double xOffset, double yOffset);
    // 滚轮点击
    void onMouseMiddleDown(double x, double y);
    // 鼠标移动
    void onMouseMove(double x, double y);
private:
    std::shared_ptr<Camera> m_camera;
    bool mMouseLeftPressed = false;
    Point2D mPosLast;
};

