#pragma once
#include <cmath>
#include <sstream>
#include "vector2.h"

namespace math {
struct vec3 {
    float x, y, z;
    
    // 构造函数
    vec3() : x(0), y(0), z(0) {}
    vec3(float v) : x(v), y(v), z(v) {}
    vec3(float x, float y, float z) : x(x), y(y), z(z) {}
    vec3(const vec2& v, float z) : x(v.x), y(v.y), z(z) {}
    explicit vec3(const float* data) : x(data[0]), y(data[1]), z(data[2]) {}
    
    // 基本运算
    vec3 operator+(const vec3& v) const { return vec3(x + v.x, y + v.y, z + v.z); }
    vec3 operator-(const vec3& v) const { return vec3(x - v.x, y - v.y, z - v.z); }
    vec3 operator*(float s) const { return vec3(x * s, y * s, z * s); }
    vec3 operator/(float s) const { return vec3(x / s, y / s, z / s); }
    vec3 operator-() const { return vec3(-x, -y, -z); }
    
    // 复合运算
    vec3& operator+=(const vec3& v) { x += v.x; y += v.y; z += v.z; return *this; }
    vec3& operator-=(const vec3& v) { x -= v.x; y -= v.y; z -= v.z; return *this; }
    vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
    vec3& operator/=(float s) { x /= s; y /= s; z /= s; return *this; }
    
    // 向量运算
    float dot(const vec3& v) const { return x * v.x + y * v.y + z * v.z; }
    vec3 cross(const vec3& v) const {
        return vec3(
                    y * v.z - z * v.y,
                    z * v.x - x * v.z,
                    x * v.y - y * v.x
                    );
    }
    
    // 归一化
    vec3 normalized() const {
        float len = length();
        return len > 0 ? (*this) / len : vec3(0.f);
    }
    
    // 长度相关
    float length() const { return std::sqrt(x * x + y * y + z * z); }
    float lengthSquared() const { return x * x + y * y + z * z; }
    
    // 实用函数
    static vec3 lerp(const vec3& a, const vec3& b, float t) {
        return a + (b - a) * t;
    }
    
    vec2 xy() const { return vec2(x, y); }
    
    static vec3 zero() { return vec3(0.f); }
    static vec3 one() { return vec3(1); }
    static vec3 up() { return vec3(0, 1, 0); }
    static vec3 right() { return vec3(1, 0, 0); }
    static vec3 forward() { return vec3(0, 0, 1); }
    
    std::string to_string() const {
        std::ostringstream oss;
        oss << "(" << x << ", " << y << ", " << z << ")";
        return oss.str();
    }
};
}
