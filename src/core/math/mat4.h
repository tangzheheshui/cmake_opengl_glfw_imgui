#pragma once
#include "vector4.h"
#include <array>
#include <cmath>
#include "vector3.h"

namespace math {
struct mat4 {
    // 列主序存储 (兼容OpenGL)
    union {
        float m[16];        // 线性数组形式
        float mm[4][4];     // 二维数组形式
        vec4 columns[4];   // 列向量形式
        struct {            // 命名元素
            float m00, m10, m20, m30;
            float m01, m11, m21, m31;
            float m02, m12, m22, m32;
            float m03, m13, m23, m33;
        };
    };

    // --- 构造函数 ---
    mat4();                             // 单位矩阵
    explicit mat4(float diagonal);      // 对角矩阵
    mat4(const vec4& col0,              // 从列向量构造
         const vec4& col1,
         const vec4& col2,
         const vec4& col3);
    
    // --- 静态构造方法 ---
    static mat4 identity();                     // 单位矩阵
    static mat4 translate(const vec3& translation);  // 平移矩阵
    static mat4 scale(const vec3& scale);            // 缩放矩阵
    static mat4 rotateX(float angle);               // X轴旋转
    static mat4 rotateY(float angle);               // Y轴旋转
    static mat4 rotateZ(float angle);               // Z轴旋转
    static mat4 rotate(const vec3& axis, float angle); // 任意轴旋转
    static mat4 lookAt(const vec3& eye,            // 视图矩阵
                      const vec3& target,
                      const vec3& up);
    static mat4 perspective(float fov,             // 透视投影
                           float aspect,
                           float near,
                           float far);
    static mat4 orthographic(float left,          // 正交投影
                            float right,
                            float bottom,
                            float top,
                            float near,
                            float far);

    // --- 运算符重载 ---
    mat4 operator*(const mat4& other) const;  // 矩阵乘法
    vec4 operator*(const vec4& v) const;      // 矩阵-向量乘法
    float* operator[](int index);             // 行访问
    const float* operator[](int index) const; // 常量行访问

    // --- 矩阵操作 ---
    mat4 transposed() const;      // 转置矩阵
    mat4 inverted() const;        // 逆矩阵 (实现略复杂，需单独讨论)
    
    // --- 实用方法 ---
    void setIdentity();           // 设为单位矩阵
    std::string to_string() const; // 字符串表示
};
}
