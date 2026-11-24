//
//  Frustum.h
//  视锥提取 + AABB 相交测试（E3 CPU 剔除；纯 CPU，core 层零依赖）
//
//  平面提取 = Gribb-Hartmann 法（列主序 VP 矩阵：数学行组合）；
//  AABB 测试 = 正顶点（p-vertex）保守测试：任一平面"正顶点在负侧"
//  即判定外部；相交一律视为可见（保守正确，不漏画）。
//
#ifndef Frustum_h
#define Frustum_h

#include <cstdint>
#include <cmath>

namespace engine {

struct Frustum {
    // 每平面 (a,b,c,d)：a*x+b*y+c*z+d >= 0 为内侧
    float planes[6][4];
    static constexpr int PLANE_LEFT = 0, PLANE_RIGHT = 1, PLANE_BOTTOM = 2,
                         PLANE_TOP = 3, PLANE_NEAR = 4, PLANE_FAR = 5;

    // 从列主序（OpenGL 约定，m[col*4 + row]）VP 矩阵提取六平面。
    // 数学行 r 的第 c 分量 = m[c*4 + r]；平面 = row3 ± rowR。
    static Frustum fromVP(const float* m) {
        Frustum f;
        auto setPlane = [&](int slot, int sign, int r) {
            float* p = f.planes[slot];
            for (int c = 0; c < 4; ++c) {
                p[c] = m[c * 4 + 3] + sign * m[c * 4 + r];
            }
        };
        setPlane(PLANE_LEFT,   +1, 0);
        setPlane(PLANE_RIGHT,  -1, 0);
        setPlane(PLANE_BOTTOM, +1, 1);
        setPlane(PLANE_TOP,    -1, 1);
        setPlane(PLANE_NEAR,   +1, 2);
        setPlane(PLANE_FAR,    -1, 2);
        for (auto& p : f.planes) {
            const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
            if (len > 1e-8f) {
                p[0] /= len; p[1] /= len; p[2] /= len; p[3] /= len;
            }
        }
        return f;
    }

    // AABB 相交测试（p-vertex 保守；true = 可见或相交）
    bool intersectsAABB(float minX, float minY, float minZ,
                        float maxX, float maxY, float maxZ) const {
        for (const auto& p : planes) {
            const float px = p[0] > 0.0f ? maxX : minX;
            const float py = p[1] > 0.0f ? maxY : minY;
            const float pz = p[2] > 0.0f ? maxZ : minZ;
            if (p[0] * px + p[1] * py + p[2] * pz + p[3] < 0.0f) {
                return false;   // 正顶点也在负侧 → 整个盒在外
            }
        }
        return true;
    }
};

} // namespace engine

#endif /* Frustum_h */
