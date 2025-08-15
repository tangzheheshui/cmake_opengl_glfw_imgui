#include "mat4.h"
#include <sstream>
#include <stdexcept>

namespace math {
mat4::mat4() {
    setIdentity();
}

mat4::mat4(float diagonal) {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = diagonal;
}

mat4::mat4(const vec4& col0, const vec4& col1, const vec4& col2, const vec4& col3) {
    columns[0] = col0;
    columns[1] = col1;
    columns[2] = col2;
    columns[3] = col3;
}

// --- 静态构造方法实现 ---
mat4 mat4::identity() {
    return mat4();
}

mat4 mat4::translate(const vec3& translation) {
    mat4 result(1.0f);
    result.m03 = translation.x;
    result.m13 = translation.y;
    result.m23 = translation.z;
    return result;
}

mat4 mat4::scale(const vec3& scale) {
    mat4 result(1.0f);
    result.m00 = scale.x;
    result.m11 = scale.y;
    result.m22 = scale.z;
    return result;
}

mat4 mat4::rotateX(float angle) {
    mat4 result(1.0f);
    float c = cos(angle);
    float s = sin(angle);
    result.m11 = c; result.m12 = -s;
    result.m21 = s; result.m22 = c;
    return result;
}

mat4 mat4::rotateY(float angle) {
    mat4 result(1.0f);
    float c = cos(angle);
    float s = sin(angle);
    result.m00 = c; result.m02 = s;
    result.m20 = -s; result.m22 = c;
    return result;
}

mat4 mat4::rotateZ(float angle) {
    mat4 result(1.0f);
    float c = cos(angle);
    float s = sin(angle);
    result.m00 = c; result.m01 = -s;
    result.m10 = s; result.m11 = c;
    return result;
}

mat4 mat4::rotate(const vec3& axis, float angle) {
    mat4 result(1.0f);
    float c = cos(angle);
    float s = sin(angle);
    float omc = 1.0f - c;
    
    vec3 normAxis = axis.normalized();
    float x = normAxis.x;
    float y = normAxis.y;
    float z = normAxis.z;
    
    result.m00 = x * x * omc + c;
    result.m01 = x * y * omc - z * s;
    result.m02 = x * z * omc + y * s;
    
    result.m10 = y * x * omc + z * s;
    result.m11 = y * y * omc + c;
    result.m12 = y * z * omc - x * s;
    
    result.m20 = z * x * omc - y * s;
    result.m21 = z * y * omc + x * s;
    result.m22 = z * z * omc + c;
    
    return result;
}

mat4 mat4::lookAt(const vec3& eye, const vec3& target, const vec3& up) {
    vec3 z = (eye - target).normalized();
    vec3 x = up.cross(z).normalized();
    vec3 y = z.cross(x);

    mat4 result(1.0f);
    result.m00 = x.x; result.m01 = x.y; result.m02 = x.z;
    result.m10 = y.x; result.m11 = y.y; result.m12 = y.z;
    result.m20 = z.x; result.m21 = z.y; result.m22 = z.z;
    
    result.m03 = -x.dot(eye);
    result.m13 = -y.dot(eye);
    result.m23 = -z.dot(eye);
    
    return result;
}

mat4 mat4::perspective(float fov, float aspect, float near, float far) {
    mat4 result(0.0f);
    float tanHalfFov = tan(fov / 2.0f);
    
    result.m00 = 1.0f / (aspect * tanHalfFov);
    result.m11 = 1.0f / tanHalfFov;
    result.m22 = -(far + near) / (far - near);
    result.m23 = -2.0f * far * near / (far - near);
    result.m32 = -1.0f;
    
    return result;
}

mat4 mat4::orthographic(float left, float right, float bottom, float top, float near, float far) {
    mat4 result(1.0f);
    result.m00 = 2.0f / (right - left);
    result.m11 = 2.0f / (top - bottom);
    result.m22 = -2.0f / (far - near);
    
    result.m03 = -(right + left) / (right - left);
    result.m13 = -(top + bottom) / (top - bottom);
    result.m23 = -(far + near) / (far - near);
    
    return result;
}

// --- 运算符重载实现 ---
mat4 mat4::operator*(const mat4& other) const {
    mat4 result(0.0f);
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            for (int k = 0; k < 4; k++) {
                result.mm[i][j] += mm[k][j] * other.mm[i][k];
            }
        }
    }
    return result;
}

vec4 mat4::operator*(const vec4& v) const {
    return vec4(
        m00 * v.x + m01 * v.y + m02 * v.z + m03 * v.w,
        m10 * v.x + m11 * v.y + m12 * v.z + m13 * v.w,
        m20 * v.x + m21 * v.y + m22 * v.z + m23 * v.w,
        m30 * v.x + m31 * v.y + m32 * v.z + m33 * v.w
    );
}

float* mat4::operator[](int index) {
    if (index < 0 || index >= 4)
        throw std::out_of_range("mat4 row index out of range");
    return mm[index];
}

const float* mat4::operator[](int index) const {
    if (index < 0 || index >= 4)
        throw std::out_of_range("mat4 row index out of range");
    return mm[index];
}

// --- 矩阵操作实现 ---
mat4 mat4::transposed() const {
    mat4 result;
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            result.mm[i][j] = mm[j][i];
        }
    }
    return result;
}

void mat4::setIdentity() {
    for (int i = 0; i < 16; i++) m[i] = 0.0f;
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

std::string mat4::to_string() const {
    std::ostringstream oss;
    oss << "mat4[\n"
        << "  " << m00 << ", " << m01 << ", " << m02 << ", " << m03 << "\n"
        << "  " << m10 << ", " << m11 << ", " << m12 << ", " << m13 << "\n"
        << "  " << m20 << ", " << m21 << ", " << m22 << ", " << m23 << "\n"
        << "  " << m30 << ", " << m31 << ", " << m32 << ", " << m33 << "\n"
        << "]";
    return oss.str();
}
}
