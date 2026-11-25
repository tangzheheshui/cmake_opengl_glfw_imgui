//
//  BaseDraw.h
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/25.
//

#ifndef BaseDraw_h
#define BaseDraw_h

#include <atomic>
#include <vector>
#include <glm/glm.hpp>
#include "RenderSystem.h"
#include "rhi/IRHIDevice.h"
#include "../core/math/matrix.h"

// E5：后台解析线程也会构造 Model（BaseDraw 子类），ID 分配需原子
inline static std::atomic<uint16_t> _staticID{0};

// 可绘制对象基类。去后端化：GPU 资源由子类经 RHI 惰性创建，
// 构造/析构不再触碰任何图形 API（无上下文的后端模式安全）。
class BaseDraw {
public:
    BaseDraw() {
        _ID = _staticID.fetch_add(1, std::memory_order_relaxed);
    }
    virtual ~BaseDraw() = default;
    virtual bool draw(const Matrix &mtx) = 0;
    virtual void update() {}
    virtual bool isClick(const glm::vec3 &worldStart, const glm::vec3 &worldEnd, float &fDeep) { return false; }
public:
    virtual void getDebugPoint(std::vector<glm::vec3> &vertices, std::vector<unsigned int> &indices) {}
    // D3：透明排序的深度键（对象世界空间参考点；默认原点，子类覆写）
    virtual glm::vec3 worldCenter() const { return glm::vec3(0.0f); }
    // D4：overlay 对象（文字等）在天空盒之后绘制——自身不写深度，
    // 先画会被天空的深度测试覆盖（两端绘制顺序契约的一段）
    virtual bool isOverlayDraw() const { return false; }

    bool getLightOpen() const { return _lightOpen; }
    void setLightOpen(const bool &b) { _lightOpen = b; }

    void setAlpha(float alpha) { _alpha = alpha; }
    float getAlpha() { return _alpha; }

    uint16_t GetID() { return _ID; }
private:
    bool _lightOpen = true;
    float _alpha = 1.0f;
    uint16_t _ID;
};

#endif /* BaseDraw_h */
