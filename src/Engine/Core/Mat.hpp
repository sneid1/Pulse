#pragma once

// Engine-side linear algebra: Vec3/Vec4 + a row-major 4x4 matrix with row-vector
// convention (v' = v * M, so MVP = model * view * proj). Shaders are compiled
// with -Zpr (row-major) so a Mat4 uploads to an HLSL float4x4 without transpose,
// and the VS does mul(float4(pos,1), MVP). Left-handed, Y-up; reverse-Z depth
// (near -> 1, far -> 0) for precision (depth clears to 0, tests GREATER_EQUAL).

#include <cmath>
#include <cstdint>

namespace pulse {

struct Vec3f { float x = 0, y = 0, z = 0; };
struct Vec4f { float x = 0, y = 0, z = 0, w = 0; };

inline Vec3f operator+(Vec3f a, Vec3f b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline Vec3f operator-(Vec3f a, Vec3f b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline Vec3f operator*(Vec3f a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float dot3(Vec3f a, Vec3f b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3f cross3(Vec3f a, Vec3f b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
inline Vec3f normalize3(Vec3f v) {
    const float l = std::sqrt(dot3(v, v));
    return l > 1e-8f ? v * (1.0f / l) : Vec3f{ 0, 0, 0 };
}

struct Mat4 {
    float m[4][4] = {};   // m[row][col], row-major

    static Mat4 identity() {
        Mat4 r;
        r.m[0][0] = r.m[1][1] = r.m[2][2] = r.m[3][3] = 1.0f;
        return r;
    }
};

// Row-vector multiply: (v * A) * B == v * (A*B), so C = A*B applies A then B.
inline Mat4 mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

inline Mat4 translation(float x, float y, float z) {
    Mat4 r = Mat4::identity();
    r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z;
    return r;
}
inline Mat4 scaling(float x, float y, float z) {
    Mat4 r = Mat4::identity();
    r.m[0][0] = x; r.m[1][1] = y; r.m[2][2] = z;
    return r;
}
inline Mat4 rotationY(float a) {
    Mat4 r = Mat4::identity();
    const float c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c;  r.m[0][2] = -s;
    r.m[2][0] = s;  r.m[2][2] = c;
    return r;
}
inline Mat4 rotationX(float a) {
    Mat4 r = Mat4::identity();
    const float c = std::cos(a), s = std::sin(a);
    r.m[1][1] = c;  r.m[1][2] = s;
    r.m[2][1] = -s; r.m[2][2] = c;
    return r;
}
inline Mat4 rotationZ(float a) {
    Mat4 r = Mat4::identity();
    const float c = std::cos(a), s = std::sin(a);
    r.m[0][0] = c;  r.m[0][1] = s;
    r.m[1][0] = -s; r.m[1][1] = c;
    return r;
}

// Yaw (Y) then pitch (X) then roll (Z), row-vector convention.
inline Mat4 rotationYawPitchRoll(float yaw, float pitch, float roll) {
    return mul(mul(rotationY(yaw), rotationX(pitch)), rotationZ(roll));
}

// Build a transform from basis vectors + origin + uniform scale (row-vector):
// a point p maps to p.x*right*scale + p.y*up*scale + p.z*forward*scale + origin.
inline Mat4 basis(Vec3f right, Vec3f up, Vec3f forward, Vec3f origin, float scale) {
    Mat4 r;
    r.m[0][0] = right.x * scale;   r.m[0][1] = right.y * scale;   r.m[0][2] = right.z * scale;
    r.m[1][0] = up.x * scale;      r.m[1][1] = up.y * scale;      r.m[1][2] = up.z * scale;
    r.m[2][0] = forward.x * scale; r.m[2][1] = forward.y * scale; r.m[2][2] = forward.z * scale;
    r.m[3][0] = origin.x;          r.m[3][1] = origin.y;          r.m[3][2] = origin.z;
    r.m[3][3] = 1.0f;
    return r;
}

// Left-handed look-at (camera looks toward +forward).
inline Mat4 lookAtLH(Vec3f eye, Vec3f target, Vec3f up) {
    const Vec3f z = normalize3(target - eye);
    const Vec3f x = normalize3(cross3(up, z));
    const Vec3f y = cross3(z, x);
    Mat4 r = Mat4::identity();
    r.m[0][0] = x.x; r.m[0][1] = y.x; r.m[0][2] = z.x;
    r.m[1][0] = x.y; r.m[1][1] = y.y; r.m[1][2] = z.y;
    r.m[2][0] = x.z; r.m[2][1] = y.z; r.m[2][2] = z.z;
    r.m[3][0] = -dot3(x, eye); r.m[3][1] = -dot3(y, eye); r.m[3][2] = -dot3(z, eye);
    return r;
}

// Left-handed perspective with infinite-far reverse-Z (near -> 1, far -> 0).
inline Mat4 perspectiveReverseZ(float fovYRadians, float aspect, float nearZ) {
    const float yScale = 1.0f / std::tan(fovYRadians * 0.5f);
    const float xScale = yScale / aspect;
    Mat4 r;
    r.m[0][0] = xScale;
    r.m[1][1] = yScale;
    r.m[2][3] = 1.0f;
    r.m[3][2] = nearZ;
    return r;
}

// Right-handed look-at (screen-right = cross(forward, up)). The game's strafe is
// rightFromForward (a right-handed convention), so the game camera uses RH so
// yaw/strafe are not mirrored. Camera looks down -Z in view space.
inline Mat4 lookAtRH(Vec3f eye, Vec3f target, Vec3f up) {
    const Vec3f f = normalize3(target - eye);
    const Vec3f r = normalize3(cross3(f, up));
    const Vec3f u = cross3(r, f);
    Mat4 m = Mat4::identity();
    m.m[0][0] = r.x; m.m[0][1] = u.x; m.m[0][2] = -f.x;
    m.m[1][0] = r.y; m.m[1][1] = u.y; m.m[1][2] = -f.y;
    m.m[2][0] = r.z; m.m[2][1] = u.z; m.m[2][2] = -f.z;
    m.m[3][0] = -dot3(r, eye); m.m[3][1] = -dot3(u, eye); m.m[3][2] = dot3(f, eye);
    return m;
}

// General 4x4 inverse via Gauss-Jordan (row-major; runs once per frame on the CPU,
// so correctness over speed). Used to reconstruct world position from depth in the
// deferred resolve. Returns identity if singular.
inline Mat4 inverse(const Mat4& m) {
    double a[4][8];
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) a[r][c] = m.m[r][c];
        for (int c = 0; c < 4; ++c) a[r][4 + c] = (r == c) ? 1.0 : 0.0;
    }
    for (int col = 0; col < 4; ++col) {
        int piv = col;
        double best = std::fabs(a[col][col]);
        for (int r = col + 1; r < 4; ++r)
            if (std::fabs(a[r][col]) > best) { best = std::fabs(a[r][col]); piv = r; }
        if (best < 1e-12) return Mat4::identity();
        if (piv != col)
            for (int c = 0; c < 8; ++c) { double t = a[col][c]; a[col][c] = a[piv][c]; a[piv][c] = t; }
        const double invp = 1.0 / a[col][col];
        for (int c = 0; c < 8; ++c) a[col][c] *= invp;
        for (int r = 0; r < 4; ++r) {
            if (r == col) continue;
            const double f = a[r][col];
            for (int c = 0; c < 8; ++c) a[r][c] -= f * a[col][c];
        }
    }
    Mat4 out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out.m[r][c] = static_cast<float>(a[r][4 + c]);
    return out;
}

// Left-handed orthographic projection mapping view z [near,far] -> clip z [0,1]
// (standard, not reverse-Z; used for the sun shadow map, sampled with LESS depth).
inline Mat4 orthographic(float width, float height, float nearZ, float farZ) {
    Mat4 r;
    r.m[0][0] = 2.0f / width;
    r.m[1][1] = 2.0f / height;
    r.m[2][2] = 1.0f / (farZ - nearZ);
    r.m[3][2] = -nearZ / (farZ - nearZ);
    r.m[3][3] = 1.0f;
    return r;
}

// Right-handed infinite-far reverse-Z perspective (pairs with lookAtRH).
inline Mat4 perspectiveReverseZRH(float fovYRadians, float aspect, float nearZ) {
    const float yScale = 1.0f / std::tan(fovYRadians * 0.5f);
    const float xScale = yScale / aspect;
    Mat4 r;
    r.m[0][0] = xScale;
    r.m[1][1] = yScale;
    r.m[2][3] = -1.0f;
    r.m[3][2] = nearZ;
    return r;
}

} // namespace pulse
