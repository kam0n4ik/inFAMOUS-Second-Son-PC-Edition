// SecondSonPC native runtime - small vector/matrix library.
//
// Conventions: right-handed world, +Z up (the game's meshes are authored Z-up). Matrices are
// column-major (m[column][row]) and multiply column vectors: v' = M * v. Projection produces
// Vulkan clip space (y down, depth 0..1).
#pragma once

#include <cmath>

namespace sspc {

struct Vec2 {
    float x = 0, y = 0;
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
    Vec3() = default;
    constexpr Vec3(float x_, float y_, float z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(const Vec3& o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(const Vec3& o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
    Vec3 operator-() const { return {-x, -y, -z}; }
    Vec3& operator+=(const Vec3& o) { x += o.x; y += o.y; z += o.z; return *this; }
    Vec3& operator-=(const Vec3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    Vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};

inline float Dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 Cross(const Vec3& a, const Vec3& b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float Length(const Vec3& v) { return std::sqrt(Dot(v, v)); }
inline Vec3 Normalize(const Vec3& v) {
    const float l = Length(v);
    return l > 1e-20f ? v * (1.0f / l) : Vec3{0, 0, 0};
}
inline Vec3 Lerp(const Vec3& a, const Vec3& b, float t) { return a + (b - a) * t; }

struct Vec4 {
    float x = 0, y = 0, z = 0, w = 0;
};

struct Mat4 {
    float m[4][4] = {};  // m[column][row]

    static Mat4 Identity() {
        Mat4 r;
        for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0f;
        return r;
    }
    static Mat4 Translation(const Vec3& t) {
        Mat4 r = Identity();
        r.m[3][0] = t.x;
        r.m[3][1] = t.y;
        r.m[3][2] = t.z;
        return r;
    }
    static Mat4 Scale(float s) {
        Mat4 r = Identity();
        r.m[0][0] = r.m[1][1] = r.m[2][2] = s;
        return r;
    }
    static Mat4 RotationZ(float radians) {
        Mat4 r = Identity();
        const float c = std::cos(radians), s = std::sin(radians);
        r.m[0][0] = c;
        r.m[0][1] = s;
        r.m[1][0] = -s;
        r.m[1][1] = c;
        return r;
    }
    // View matrix looking from eye to target, +Z up.
    static Mat4 LookAt(const Vec3& eye, const Vec3& target, const Vec3& up) {
        const Vec3 f = Normalize(target - eye);
        const Vec3 s = Normalize(Cross(f, up));
        const Vec3 u = Cross(s, f);
        Mat4 r = Identity();
        r.m[0][0] = s.x; r.m[1][0] = s.y; r.m[2][0] = s.z;
        r.m[0][1] = u.x; r.m[1][1] = u.y; r.m[2][1] = u.z;
        r.m[0][2] = -f.x; r.m[1][2] = -f.y; r.m[2][2] = -f.z;
        r.m[3][0] = -Dot(s, eye);
        r.m[3][1] = -Dot(u, eye);
        r.m[3][2] = Dot(f, eye);
        return r;
    }
    // Right-handed perspective to Vulkan clip space: depth 0 at near, 1 at far, y flipped.
    static Mat4 Perspective(float fov_y_radians, float aspect, float near_z, float far_z) {
        const float f = 1.0f / std::tan(fov_y_radians * 0.5f);
        Mat4 r;
        r.m[0][0] = f / aspect;
        r.m[1][1] = -f;
        r.m[2][2] = far_z / (near_z - far_z);
        r.m[2][3] = -1.0f;
        r.m[3][2] = (near_z * far_z) / (near_z - far_z);
        return r;
    }

    Mat4 operator*(const Mat4& o) const {
        Mat4 r;
        for (int c = 0; c < 4; ++c)
            for (int rr = 0; rr < 4; ++rr) {
                float s = 0;
                for (int k = 0; k < 4; ++k) s += m[k][rr] * o.m[c][k];
                r.m[c][rr] = s;
            }
        return r;
    }
    Vec3 TransformPoint(const Vec3& p) const {
        return {m[0][0] * p.x + m[1][0] * p.y + m[2][0] * p.z + m[3][0],
                m[0][1] * p.x + m[1][1] * p.y + m[2][1] * p.z + m[3][1],
                m[0][2] * p.x + m[1][2] * p.y + m[2][2] * p.z + m[3][2]};
    }
};

constexpr float kPi = 3.14159265358979323846f;

}  // namespace sspc
