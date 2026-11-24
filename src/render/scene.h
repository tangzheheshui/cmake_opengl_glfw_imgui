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
    // D3：透明排序绘制列表——不透明对象按加入顺序在前，透明对象（alpha < 1）
    // 按到相机距离降序（远 → 近）在后。两端绘制路径共用，保证 GL/VK 画序一致。
    std::vector<std::shared_ptr<BaseDraw>> getSortedDrawList(const glm::vec3& camPos) const;
private:
    // camera
    std::shared_ptr<Camera> mCameraActive;
    std::shared_ptr<CCameraController> mCameraController;
    // 天空盒
    std::shared_ptr<Sky> mSkyBox;
    // 渲染对象
    std::vector<std::shared_ptr<BaseDraw>> m_vec_drawobj;
    std::vector<std::shared_ptr<BaseDraw>> m_vec_drawobj_blend;
    // 光源矩阵
    inline static Matrix _lightVPMatrix;
};

#endif /* scene_hpp */
