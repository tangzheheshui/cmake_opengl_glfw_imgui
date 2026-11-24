//
//  GLProgram.h
//  RHI OpenGL 后端 - GLSL 编译/链接 + uniform 设置
//
//  L0（docs/ARCHITECTURE_INTERFACES.md §3.7）：把 render/shader.h 的 GLSL
//  编译逻辑下沉进 RHI，src/rhi 不再依赖 render 层。
//  与旧 Shader 类的差异：
//  - uniform location 首次查询后缓存（热路径省 glGetUniformLocation）
//  - 编译/链接失败返回 false + 日志（不再 assert(0) 中止进程）
//  - setMat4/setMat4Array 直接接收原始 float*（去 Matrix/glm 依赖）
//
#ifndef GLProgram_h
#define GLProgram_h

#include <string>
#include <unordered_map>

#include <glad/glad.h>

namespace rhi {

class GLProgram {
public:
    GLProgram() = default;
    ~GLProgram();

    GLProgram(const GLProgram&) = delete;
    GLProgram& operator=(const GLProgram&) = delete;

    // 编译 + 链接（vert/frag 必需；geom 可选——注意 MoltenVK 不支持几何着色器，
    // GL 侧保留能力）。任一阶段失败返回 false 并记录日志，program 置 0。
    bool compile(const std::string& vertPath,
                 const std::string& fragPath,
                 const std::string& geomPath = "");

    void use() const;
    GLuint id() const { return _id; }
    bool   valid() const { return _id != 0; }

    // ---- uniform 设置（location 缓存）----
    void setBool(const char* name, bool v) const;
    void setInt(const char* name, int v) const;
    void setFloat(const char* name, float v) const;
    void setFloat3(const char* name, float x, float y, float z) const;
    void setFloat4(const char* name, float x, float y, float z, float w) const;
    void setMat4(const char* name, const float m[16]) const;
    void setMat4Array(const char* name, const float* m, int count) const;

private:
    GLint loc(const char* name) const;   // 首查缓存

    GLuint _id = 0;
    mutable std::unordered_map<std::string, GLint> _locs;
};

} // namespace rhi

#endif /* GLProgram_h */
