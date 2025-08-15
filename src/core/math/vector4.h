#pragma once
#include <cmath>
#include <sstream>
#include "vector3.h"

namespace math {
struct vec4 {
    float x, y, z, w;
    
    // 构造函数
    vec4() : x(0), y(0), z(0), w(0) {}
    vec4(float v) : x(v), y(v), z(v), w(v) {}
    vec4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
    vec4(const vec3& v, float w) : x(v.x), y(v.y), z(v.z), w(w) {}
    explicit vec4(const float* data) : x(data[0]), y(data[1]), z(data[2]), w(data[3]) {}
    
    // 基本运算
    vec4 operator+(const vec4& v) const { return vec4(x + v.x, y + v.y, z + v.z, w + v.w); }
    vec4 operator-(const vec4& v) const { return vec4(x - v.x, y - v.y, z - v.z, w - v.w); }
    vec4 operator*(float s) const { return vec4(x * s, y * s, z * s, w * s); }
    vec4 operator/(float s) const { return vec4(x / s, y / s, z / s, w / s); }
    vec4 operator-() const { return vec4(-x, -y, -z, -w); }
    
    // 复合运算
    vec4& operator+=(const vec4& v) { x += v.x; y += v.y; z += v.z; w += v.w; return *this; }
    vec4& operator-=(const vec4& v) { x -= v.x; y -= v.y; z -= v.z; w -= v.w; return *this; }
    vec4& operator*=(float s) { x *= s; y *= s; z *= s; w *= s; return *this; }
    vec4& operator/=(float s) { x /= s; y /= s; z /= s; w /= s; return *this; }
    
    // 向量运算
    float dot(const vec4& v) const { return x * v.x + y * v.y + z * v.z + w * v.w; }
    
    // 归一化
    vec4 normalized() const {
        float len = length();
        return len > 0 ? (*this) / len : vec4(0.f);
    }
    
    // 长度相关
    float length() const { return std::sqrt(x * x + y * y + z * z + w * w); }
    float lengthSquared() const { return x * x + y * y + z * z + w * w; }
    
    // 实用函数
    static vec4 lerp(const vec4& a, const vec4& b, float t) {
        return a + (b - a) * t;
    }
    
    vec3 xyz() const { return vec3(x, y, z); }
    
    static vec4 zero() { return vec4(0.f); }
    static vec4 one() { return vec4(1); }
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "(" << x << ", " << y << ", " << z << ", " << w <<")";
        return oss.str();
    }
};
}
