//
//  GLProgram.cpp
//
#include "GLProgram.h"
#include "log/LogMacros.h"
#include "fs/FilePathUtil.h"

#include <fstream>
#include <sstream>

namespace rhi {

namespace {

// A4：路径按 UTF-8 解释，整文件读入字符串
bool readFileText(const std::string& path, std::string& out) {
    std::ifstream f(engine::fs::pathFromUtf8(path));
    if (!f.is_open()) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// 编译单个着色器阶段；失败打印 infoLog 并返回 0
GLuint compileStage(GLenum stage, const char* source, const char* label) {
    GLuint shader = glCreateShader(stage);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        ENGINE_LOG_ERROR("[GL] %s compile failed: %s", label, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

} // namespace

GLProgram::~GLProgram() {
    if (_id) glDeleteProgram(_id);
}

bool GLProgram::compile(const std::string& vertPath,
                        const std::string& fragPath,
                        const std::string& geomPath) {
    std::string vertSrc, fragSrc, geomSrc;
    if (!readFileText(vertPath, vertSrc)) {
        ENGINE_LOG_ERROR("[GL] cannot open vertex shader: %s", vertPath.c_str());
        return false;
    }
    if (!readFileText(fragPath, fragSrc)) {
        ENGINE_LOG_ERROR("[GL] cannot open fragment shader: %s", fragPath.c_str());
        return false;
    }

    const GLuint vs = compileStage(GL_VERTEX_SHADER, vertSrc.c_str(), "vertex");
    const GLuint fs = compileStage(GL_FRAGMENT_SHADER, fragSrc.c_str(), "fragment");
    GLuint gs = 0;
    if (!geomPath.empty()) {
        if (readFileText(geomPath, geomSrc)) {
            gs = compileStage(GL_GEOMETRY_SHADER, geomSrc.c_str(), "geometry");
        } else {
            ENGINE_LOG_ERROR("[GL] cannot open geometry shader: %s", geomPath.c_str());
        }
    }
    if (!vs || !fs || (!geomPath.empty() && !gs)) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        if (gs) glDeleteShader(gs);
        return false;
    }

    if (_id) glDeleteProgram(_id);
    _id = glCreateProgram();
    glAttachShader(_id, vs);
    glAttachShader(_id, fs);
    if (gs) glAttachShader(_id, gs);
    glLinkProgram(_id);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (gs) glDeleteShader(gs);

    GLint ok = GL_FALSE;
    glGetProgramiv(_id, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        glGetProgramInfoLog(_id, sizeof(log), nullptr, log);
        ENGINE_LOG_ERROR("[GL] program link failed (%s + %s): %s",
                         vertPath.c_str(), fragPath.c_str(), log);
        glDeleteProgram(_id);
        _id = 0;
        return false;
    }
    _locs.clear();
    return true;
}

void GLProgram::use() const {
    if (_id) glUseProgram(_id);
}

GLint GLProgram::loc(const char* name) const {
    auto it = _locs.find(name);
    if (it != _locs.end()) return it->second;
    const GLint l = _id ? glGetUniformLocation(_id, name) : -1;
    _locs.emplace(name, l);
    return l;
}

void GLProgram::setBool(const char* name, bool v) const {
    glUniform1i(loc(name), v ? 1 : 0);
}

void GLProgram::setInt(const char* name, int v) const {
    glUniform1i(loc(name), v);
}

void GLProgram::setFloat(const char* name, float v) const {
    glUniform1f(loc(name), v);
}

void GLProgram::setFloat3(const char* name, float x, float y, float z) const {
    glUniform3f(loc(name), x, y, z);
}

void GLProgram::setFloat4(const char* name, float x, float y, float z, float w) const {
    glUniform4f(loc(name), x, y, z, w);
}

void GLProgram::setMat4(const char* name, const float m[16]) const {
    glUniformMatrix4fv(loc(name), 1, GL_FALSE, m);
}

void GLProgram::setMat4Array(const char* name, const float* m, int count) const {
    if (count > 0) glUniformMatrix4fv(loc(name), count, GL_FALSE, m);
}

} // namespace rhi
