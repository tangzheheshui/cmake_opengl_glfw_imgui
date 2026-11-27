//
//  Light.h
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/24.
//

#ifndef Light_h
#define Light_h

#include <glm/glm.hpp>

class Light {
public:
    static Light& GlobalLight() {
        static Light light;
        static bool init = false;
        if (!init) {   // 只初始化一次，允许场景后续 setPosition 覆盖方向
            light.position = {0.f, 10.0f, .0f};
            light.direction = -light.position;
            light.cosTheta = cos(degrees_to_radians(15));
            light.ambient = glm::vec3(0.3f);
            light.diffuse = glm::vec3(1.0f);   // PBR 直接光强度
            light.specular = glm::vec3(0.f);
            init = true;
        }
        return light;
    }
    // 场景可覆盖光源位置（direction 同步为 -position，保持锥形方向语义）
    void setPosition(const glm::vec3& p) { position = p; direction = -p; }
public:
    glm::vec3 position;
    glm::vec3 direction;
    float cosTheta = 0.f;
    glm::vec3 ambient;
    glm::vec3 diffuse;
    glm::vec3 specular;
};

#endif /* Light_h */
