//
//  shader.hpp
//  OpenglLearn
//
//  Created by liuhaifeng02 on 2024/1/1.
//

#ifndef shader_hpp
#define shader_hpp

#include <string>
#include <map>
#include <vector>
#include <glm/mat4x4.hpp>
#include "math/matrix.h"

class Shader {
public:
    Shader(const std::string &vertPath, const std::string &fragPath, const std::string &geomPath = "");
    ~Shader();
    // use
    void use();
    
    // set uniform
    void setBool(const std::string &name, bool value) const;  
    void setInt(const std::string &name, int value) const;   
    void setFloat(const std::string &name, float value) const;
    void setFloat3(const std::string &name, float v1, float v2, float v3) const;
    void setFloat4(const std::string &name, float v1, float v2, float v3, float v4) const;
    void setMat4Array(const std::string &name, const std::vector<Matrix> &value) const;
    void setMat4(const std::string &name, const Matrix &value) const;
    
private:
    // 程序ID
    unsigned int _ID;
};

enum class ShaderType {
    Invalid = -1,
    Image,
    Color,
    Model_Color,
    Model_Texture,
    Model_Texture_MultiView,
    Shadow_Color,
    Shadow_Texture,
    Debug_DeepTexture,
    Ground,
    Model_Color_Anim,
    Model_Texture_Anim,
    Sky,
};

class ShaderCache {
public:
    static ShaderCache& GetInstance();
    Shader* GetShader(ShaderType type);
private:
    ShaderCache();
private:
    std::map<ShaderType, Shader*> m_map_shader;
};

#endif
