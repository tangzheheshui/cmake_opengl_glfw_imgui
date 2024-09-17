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

namespace fs = std::filesystem;

class BaseDraw;
class Line;
class Shader;
class ImageRectangle;
class CCamera;

class Scene {
private:
    Scene();
    void createObjs();
    void loadTexture(const fs::path& dirPath);
public:
    static Scene& getScene();
    static Matrix GetLightVPMatrix();
    void drawShadow();
    void draw();
    void update();
    void setLightUniform(Shader* shader);
    void processMouseClick(double x, double y);
    // �������
    void cameraZoom(float scale);
    void cameraRotate();
    void cameraDrag();
private:
    std::shared_ptr<Line> getTestLine();
    std::vector<std::shared_ptr<ImageRectangle>> createGlass();
    void creatBlendTexture();
    void clearBlendTexture();
    void set4Viewport();
private:
    std::vector<std::shared_ptr<BaseDraw>> m_vec_drawobj;
    std::vector<std::shared_ptr<BaseDraw>> m_vec_drawobj_blend;
    GLuint _depthMapFBO = 0;
    inline static Matrix* _lightVPMatrix = nullptr;
    
    GLuint _fragment_alpha_buffer;
    GLuint _atomic_counter;
    GLuint _texture_blend;
    GLuint _pbo_head_pointer;
    std::shared_ptr<CCamera> m_camera;
};

#endif /* scene_hpp */
