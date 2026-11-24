//
//  BaseDraw.h
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/25.
//

#ifndef BaseDraw_h
#define BaseDraw_h

#include <glad/glad.h>
#include <vector>
#include <glm/glm.hpp>
#include "RenderSystem.h"
#include "rhi/IRHIDevice.h"
#include "../core/math/matrix.h"

inline static uint16_t _staticID = 0;

class BaseDraw {
public:
    BaseDraw() {
        _ID = _staticID++;
        // 引擎原则：构造函数禁止在非 GL 后端触碰 GL（Vulkan 模式无 GL 上下文，
        // glad 函数指针为空，任何 gl* 调用都会跳空地址）
        auto* device = RenderSystem::getInstance().rhiDevice();
        if (!device || device->backend() == rhi::Backend::OpenGL) {
            glGenVertexArrays(1, &_VAO);
            glGenBuffers(1, &_VBO);
            glGenBuffers(1, &_EBO);
        }
    }
    virtual ~BaseDraw() {
        if (_VAO > 0) {
            glDeleteVertexArrays(1, &_VAO);
        }

        if (_VBO > 0) {
            glDeleteBuffers(1, &_VBO);
        }

        if (_EBO > 0) {
            glDeleteBuffers(1, &_EBO);
        }
    }
    virtual bool draw(const Matrix &mtx) = 0;
    virtual bool drawShadow() { return false; }
    virtual bool drawPick() { return false; }
    virtual void update() {}
    virtual bool isClick(const glm::vec3 &worldStart, const glm::vec3 &worldEnd, float &fDeep) { return false; }
public:
    virtual void getDebugPoint(std::vector<glm::vec3> &vertices, std::vector<unsigned int> &indices) {}
    // D3：透明排序的深度键（对象世界空间参考点；默认原点，子类覆写）
    virtual glm::vec3 worldCenter() const { return glm::vec3(0.0f); }
    
    bool getLightOpen() const { return _lightOpen; }
    void setLightOpen(const bool &b) { _lightOpen = b; }
    
    void setAlpha(float alpha) { _alpha = alpha; }
    float getAlpha() { return _alpha; }
    
    uint16_t GetID() { return _ID; }
protected:
    unsigned int _VAO = 0;
    unsigned int _VBO = 0;
    unsigned int _EBO = 0;
    bool _has_bind_vbo = false;
private:
    bool _lightOpen = true;
    float _alpha = 1.0f;
    uint16_t _ID;
};

#endif /* BaseDraw_h */
