//
//  scene.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/25.
//

#ifndef scene_hpp
#define scene_hpp

#include <filesystem>
#include <vector>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "math/matrix.h"

class BaseDraw;
class Line;
class Shader;
class ImageRectangle;
class Sky;
class Camera;
class CCameraController;

class Scene {
public:
    static Matrix GetLightVPMatrix();
    Scene();
    void init();
    std::shared_ptr<Camera> GetActiveCamera() { return mCameraActive; }
    std::shared_ptr<CCameraController> GetActiveCameraController() { return mCameraController; }

    std::shared_ptr<Sky> GetSkyBox() { return mSkyBox; }
    void SetSkyBox(std::shared_ptr<Sky> sky) { mSkyBox = sky; }

    std::vector<std::shared_ptr<BaseDraw>> GetObjs() { return m_vec_drawobj; }
    void AddObj(std::shared_ptr<BaseDraw> obj);
    // C2 收尾：多相机视口——tile i>0 依次使用加入的相机（tile 0 恒为主相机）
    void addViewportCamera(std::shared_ptr<Camera> cam) { m_viewportCameras.push_back(cam); }
    const std::vector<std::shared_ptr<Camera>>& getViewportCameras() const { return m_viewportCameras; }
    // D3：透明排序绘制列表——不透明对象按加入顺序在前，透明对象（alpha < 1）
    // 按到相机距离降序（远 → 近）在后。两端绘制路径共用，保证 GL/VK 画序一致。
    // E3：vp 非空时做视锥剔除（RenderSettings::frustumCull 开关），统计在
    // lastCulledCount()；vp 传 nullptr = 不剔除（调试对比）。
    std::vector<std::shared_ptr<BaseDraw>> getSortedDrawList(const glm::vec3& camPos,
                                                             const void* vpFloat16 = nullptr) const;
    // D3/X-ray：透明分相谓词（X-ray 开启时模型整体视为透明），各绘制路径共用
    static bool isTransparentDraw(const std::shared_ptr<BaseDraw>& obj);
    // E3：最近一次 getSortedDrawList 剔除统计（调试面板/日志）
    uint32_t lastCulledCount() const { return _lastCulled; }
private:
    // camera
    std::shared_ptr<Camera> mCameraActive;
    std::shared_ptr<CCameraController> mCameraController;
    // 天空盒
    std::shared_ptr<Sky> mSkyBox;
    // 渲染对象
    std::vector<std::shared_ptr<BaseDraw>> m_vec_drawobj;
    std::vector<std::shared_ptr<BaseDraw>> m_vec_drawobj_blend;
    // E3：上次剔除计数（mutable：getSortedDrawList 是 const）
    mutable uint32_t _lastCulled = 0;
    // C2：多相机视口相机表
    std::vector<std::shared_ptr<Camera>> m_viewportCameras;
    // 光源矩阵
    inline static Matrix _lightVPMatrix;
};

#endif /* scene_hpp */
