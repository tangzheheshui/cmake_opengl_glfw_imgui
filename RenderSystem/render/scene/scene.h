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
    static Scene& getScene();
    static Matrix GetLightVPMatrix();
    Scene();
    void setRootPath(const std::string &path) { m_root_path = path; }
    void init();
    std::shared_ptr<Camera> GetActiveCamera() { return mCameraActive; }
    std::shared_ptr<CCameraController> GetActiveCameraController() { return mCameraController; }

    std::shared_ptr<Sky> GetSkyBox() { return mSkyBox; }
    void SetSkyBox(std::shared_ptr<Sky> sky) { mSkyBox = sky; }

    std::vector<std::shared_ptr<BaseDraw>> GetObjs() { return m_vec_drawobj; }
    void AddObj(std::shared_ptr<BaseDraw> obj);
private:
    std::shared_ptr<Line> getTestLine();
    std::vector<std::shared_ptr<ImageRectangle>> createGlass();
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
    inline static Matrix* _lightVPMatrix = nullptr;
    // 路径
    std::string m_root_path;
};

#endif /* scene_hpp */
