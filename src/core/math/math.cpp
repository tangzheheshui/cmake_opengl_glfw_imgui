//
//  math.cpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/3/3.
//

#include "math.h"

glm::vec3 Math::intersectLinePlane(const glm::vec3& pointOnPlane, const glm::vec3& planeNormal, const glm::vec3& linePoint, const glm::vec3& lineDirection) {
    float d = glm::dot(pointOnPlane - linePoint, planeNormal) / glm::dot(lineDirection, planeNormal);
    return linePoint + d * lineDirection;
}

// 判断点是否在三维多边形内�?
bool Math::isInsidePolygon(const std::vector<glm::vec3>& polygon, const glm::vec3& point) {
    size_t n = polygon.size();
    glm::vec3 normal(0.0f, 0.0f, 0.0f);

    // 计算多边形的法向�?
    glm::vec3 oldNormal;
    for (int i = 0; i < n; ++i) {
        int j = (i + 1) % n;
        glm::vec3 normal = glm::cross(polygon[j]-polygon[i], point - polygon[i]);
        if (i == 0) {
            oldNormal = normal;
        }
        
        if (glm::dot(normal, oldNormal) < 0) {
            return false;
        }
    }
    return true;
}
