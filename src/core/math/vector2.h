#pragma once
#include <cmath>
#include <sstream>

namespace math {
struct vec2 {
    float x, y;
    
    // 构造函数
    vec2() : x(0), y(0) {}
    vec2(float v) : x(v), y(v) {}
    vec2(float x, float y) : x(x), y(y) {}
    explicit vec2(const float* data) : x(data[0]), y(data[1]) {}
    
    // 基本运算
    vec2 operator+(const vec2& v) const { return vec2(x + v.x, y + v.y); }
    vec2 operator-(const vec2& v) const { return vec2(x - v.x, y - v.y); }
    vec2 operator*(float s) const { return vec2(x * s, y * s); }
    vec2 operator/(float s) const { return vec2(x / s, y / s); }
    vec2 operator-() const { return vec2(-x, -y); }
    
    // 复合运算
    vec2& operator+=(const vec2& v) { x += v.x; y += v.y; return *this; }
    vec2& operator-=(const vec2& v) { x -= v.x; y -= v.y; return *this; }
    vec2& operator*=(float s) { x *= s; y *= s; return *this; }
    vec2& operator/=(float s) { x /= s; y /= s; return *this; }
    
    // 向量运算
    float dot(const vec2& v) const { return x * v.x + y * v.y; }
    float cross(const vec2& v) const { return x * v.y - y * v.x; }
    
    // 归一化
    vec2 normalized() const {
        float len = length();
        return len > 0 ? (*this) / len : vec2(0.f);
    }
    
    // 长度相关
    float length() const { return std::sqrt(x * x + y * y); }
    float lengthSquared() const { return x * x + y * y; }
    
    // 实用函数
    static vec2 lerp(const vec2& a, const vec2& b, float t) {
        return a + (b - a) * t;
    }
    static vec2 zero() { return vec2(0.f); }
    static vec2 one() { return vec2(1); }
    static vec2 right() { return vec2(1, 0); }
    static vec2 up() { return vec2(0, 1); }
    std::string to_string() const {
        std::ostringstream oss;
        oss << "(" << x << ", " << y << ")";
        return oss.str();
    }
};
}
