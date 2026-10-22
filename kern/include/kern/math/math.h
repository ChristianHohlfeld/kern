// Kern math: vectors, quaternions and column-major 4x4 matrices (OpenGL
// layout). World convention: metres, right-handed, Z up.
//
// Everything is header-only and allocation free so the compiler can inline
// and vectorise the hot paths (instance matrices, culling, picking).
#pragma once

#include <cmath>

#include "kern/core/base.h"

namespace kern {

constexpr f32 kPi = 3.14159265358979323846f;

struct Vec2 {
  f32 x = 0, y = 0;
  constexpr Vec2() = default;
  constexpr Vec2(f32 x_, f32 y_) : x(x_), y(y_) {}
  constexpr Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
  constexpr Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
  constexpr Vec2 operator*(f32 s) const { return {x * s, y * s}; }
  constexpr Vec2& operator+=(Vec2 o) { x += o.x; y += o.y; return *this; }
};
constexpr f32 dot(Vec2 a, Vec2 b) { return a.x * b.x + a.y * b.y; }
constexpr f32 cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
inline f32 length(Vec2 a) { return std::sqrt(dot(a, a)); }

struct Vec3 {
  f32 x = 0, y = 0, z = 0;
  constexpr Vec3() = default;
  constexpr Vec3(f32 x_, f32 y_, f32 z_) : x(x_), y(y_), z(z_) {}
  static Vec3 load(const f32* p) { return {p[0], p[1], p[2]}; }
  void store(f32* p) const { p[0] = x; p[1] = y; p[2] = z; }
  constexpr f32 operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
  constexpr f32& operator[](int i) { return i == 0 ? x : (i == 1 ? y : z); }
  constexpr Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
  constexpr Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
  constexpr Vec3 operator-() const { return {-x, -y, -z}; }
  constexpr Vec3 operator*(f32 s) const { return {x * s, y * s, z * s}; }
  constexpr Vec3 operator*(Vec3 o) const { return {x * o.x, y * o.y, z * o.z}; }
  constexpr Vec3 operator/(f32 s) const { return {x / s, y / s, z / s}; }
  constexpr Vec3& operator+=(Vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
  constexpr Vec3& operator-=(Vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
  constexpr Vec3& operator*=(f32 s) { x *= s; y *= s; z *= s; return *this; }
  constexpr bool operator==(const Vec3& o) const { return x == o.x && y == o.y && z == o.z; }
};
constexpr f32 dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
constexpr Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline f32 length(Vec3 a) { return std::sqrt(dot(a, a)); }
constexpr f32 length_sq(Vec3 a) { return dot(a, a); }
inline f32 distance(Vec3 a, Vec3 b) { return length(a - b); }
constexpr f32 distance_sq(Vec3 a, Vec3 b) { return length_sq(a - b); }
inline bool finite(Vec3 a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
constexpr Vec3 lerp(Vec3 a, Vec3 b, f32 t) { return a + (b - a) * t; }
constexpr Vec3 vmin(Vec3 a, Vec3 b) { return {min_of(a.x, b.x), min_of(a.y, b.y), min_of(a.z, b.z)}; }
constexpr Vec3 vmax(Vec3 a, Vec3 b) { return {max_of(a.x, b.x), max_of(a.y, b.y), max_of(a.z, b.z)}; }
/// Unit vector, or `fallback` for (near) zero input.
inline Vec3 normalize(Vec3 a, Vec3 fallback = {0, 0, 1}) {
  const f32 n = length(a);
  return n > 1e-9f ? a / n : fallback;
}
/// 0xRRGGBB to 0..1 RGB (no colour space change).
constexpr Vec3 hex_color(u32 h) {
  return {((h >> 16) & 255) / 255.0f, ((h >> 8) & 255) / 255.0f, (h & 255) / 255.0f};
}

struct Vec4 {
  f32 x = 0, y = 0, z = 0, w = 0;
  constexpr Vec4() = default;
  constexpr Vec4(f32 x_, f32 y_, f32 z_, f32 w_) : x(x_), y(y_), z(z_), w(w_) {}
  constexpr Vec4(Vec3 v, f32 w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
  constexpr Vec3 xyz() const { return {x, y, z}; }
  constexpr bool operator==(const Vec4& o) const { return x == o.x && y == o.y && z == o.z && w == o.w; }
};

/// Unit quaternion (x, y, z, w).
struct Quat {
  f32 x = 0, y = 0, z = 0, w = 1;
  static Quat axis_angle(Vec3 axis, f32 radians) {
    const Vec3 a = normalize(axis);
    const f32 s = std::sin(radians * 0.5f);
    return {a.x * s, a.y * s, a.z * s, std::cos(radians * 0.5f)};
  }
  Quat operator*(const Quat& b) const {
    return {w * b.x + x * b.w + y * b.z - z * b.y, w * b.y - x * b.z + y * b.w + z * b.x,
            w * b.z + x * b.y - y * b.x + z * b.w, w * b.w - x * b.x - y * b.y - z * b.z};
  }
  Vec3 rotate(Vec3 v) const {
    const Vec3 q{x, y, z};
    const Vec3 t = cross(q, v) * 2.0f;
    return v + t * w + cross(q, t);
  }
};

/// Column-major 4x4 matrix: element (row r, column c) is m[c * 4 + r].
struct Mat4 {
  f32 m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

  static Mat4 identity() { return {}; }
  static Mat4 load(const f32* p) { Mat4 r; std::memcpy(r.m, p, sizeof r.m); return r; }
  f32& operator[](int i) { return m[i]; }
  f32 operator[](int i) const { return m[i]; }
  Vec3 translation() const { return {m[12], m[13], m[14]}; }
  bool is_identity() const {
    static const Mat4 id;
    return std::memcmp(m, id.m, sizeof m) == 0;
  }

  Mat4 operator*(const Mat4& b) const {
    Mat4 r;
    for (int c = 0; c < 4; ++c) {
      const f32 b0 = b.m[4 * c], b1 = b.m[4 * c + 1], b2 = b.m[4 * c + 2], b3 = b.m[4 * c + 3];
      for (int k = 0; k < 4; ++k)
        r.m[4 * c + k] = m[k] * b0 + m[4 + k] * b1 + m[8 + k] * b2 + m[12 + k] * b3;
    }
    return r;
  }
  Vec3 transform_point(Vec3 p) const {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12], m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13],
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14]};
  }
  Vec3 transform_vector(Vec3 v) const {
    return {m[0] * v.x + m[4] * v.y + m[8] * v.z, m[1] * v.x + m[5] * v.y + m[9] * v.z,
            m[2] * v.x + m[6] * v.y + m[10] * v.z};
  }
  Vec4 transform(Vec4 p) const {
    return {m[0] * p.x + m[4] * p.y + m[8] * p.z + m[12] * p.w, m[1] * p.x + m[5] * p.y + m[9] * p.z + m[13] * p.w,
            m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14] * p.w, m[3] * p.x + m[7] * p.y + m[11] * p.z + m[15] * p.w};
  }

  static Mat4 translate(Vec3 t) { Mat4 r; r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; return r; }
  static Mat4 scale(Vec3 s) { Mat4 r; r.m[0] = s.x; r.m[5] = s.y; r.m[10] = s.z; return r; }

  /// Translation * rotation * scale.
  static Mat4 trs(Vec3 t, Quat q, Vec3 s) {
    const f32 x2 = q.x + q.x, y2 = q.y + q.y, z2 = q.z + q.z;
    const f32 xx = q.x * x2, xy = q.x * y2, xz = q.x * z2, yy = q.y * y2, yz = q.y * z2, zz = q.z * z2;
    const f32 wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    Mat4 r;
    r.m[0] = (1 - yy - zz) * s.x; r.m[1] = (xy + wz) * s.x; r.m[2] = (xz - wy) * s.x; r.m[3] = 0;
    r.m[4] = (xy - wz) * s.y; r.m[5] = (1 - xx - zz) * s.y; r.m[6] = (yz + wx) * s.y; r.m[7] = 0;
    r.m[8] = (xz + wy) * s.z; r.m[9] = (yz - wx) * s.z; r.m[10] = (1 - xx - yy) * s.z; r.m[11] = 0;
    r.m[12] = t.x; r.m[13] = t.y; r.m[14] = t.z; r.m[15] = 1;
    return r;
  }

  static Mat4 perspective(f32 fov, f32 aspect, f32 near, f32 far) {
    const f32 f = 1.0f / std::tan(fov * 0.5f), nf = 1.0f / (near - far);
    Mat4 r;
    std::memset(r.m, 0, sizeof r.m);
    r.m[0] = f / aspect; r.m[5] = f; r.m[10] = (far + near) * nf; r.m[11] = -1; r.m[14] = 2 * far * near * nf;
    return r;
  }

  static Mat4 ortho(f32 l, f32 r_, f32 b, f32 t, f32 n, f32 f) {
    Mat4 r;
    std::memset(r.m, 0, sizeof r.m);
    r.m[0] = 2 / (r_ - l); r.m[5] = 2 / (t - b); r.m[10] = -2 / (f - n);
    r.m[12] = -(r_ + l) / (r_ - l); r.m[13] = -(t + b) / (t - b); r.m[14] = -(f + n) / (f - n); r.m[15] = 1;
    return r;
  }

  /// View matrix. Returns identity (and false in `ok`) for a degenerate basis.
  static Mat4 look_at(Vec3 eye, Vec3 target, Vec3 up, bool* ok = nullptr) {
    Vec3 z = eye - target;
    f32 len = length(z);
    Mat4 r;
    if (!(len > 1e-10f)) { if (ok) *ok = false; return r; }
    z = z / len;
    Vec3 x = cross(up, z);
    len = length(x);
    if (!(len > 1e-10f)) { if (ok) *ok = false; return r; }
    x = x / len;
    const Vec3 y = cross(z, x);
    r.m[0] = x.x; r.m[1] = y.x; r.m[2] = z.x; r.m[3] = 0;
    r.m[4] = x.y; r.m[5] = y.y; r.m[6] = z.y; r.m[7] = 0;
    r.m[8] = x.z; r.m[9] = y.z; r.m[10] = z.z; r.m[11] = 0;
    r.m[12] = -dot(x, eye); r.m[13] = -dot(y, eye); r.m[14] = -dot(z, eye); r.m[15] = 1;
    if (ok) *ok = true;
    return r;
  }
};

/// Inverse-transpose of the upper 3x3 of `m` (column-major 3x3 into out[9]).
/// Returns false for a singular matrix (out is left as identity).
inline bool normal_matrix3(const f32* m, f32* out) {
  const f32 a = m[0], b = m[4], c = m[8];
  const f32 d = m[1], e = m[5], f = m[9];
  const f32 g = m[2], h = m[6], i = m[10];
  const f32 A = e * i - f * h, B = f * g - d * i, C = d * h - e * g;
  const f32 det = a * A + b * B + c * C;
  if (!(std::fabs(det) >= 1e-10f)) {
    static constexpr f32 id[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::memcpy(out, id, sizeof id);
    return false;
  }
  const f32 inv = 1.0f / det;
  out[0] = A * inv; out[1] = (c * h - b * i) * inv; out[2] = (b * f - c * e) * inv;
  out[3] = B * inv; out[4] = (a * i - c * g) * inv; out[5] = (c * d - a * f) * inv;
  out[6] = C * inv; out[7] = (b * g - a * h) * inv; out[8] = (a * e - b * d) * inv;
  return true;
}

/// General inverse in double precision. Returns false when singular.
inline bool invert(const Mat4& src, f64 out[16]) {
  const f64 a00 = src.m[0], a01 = src.m[1], a02 = src.m[2], a03 = src.m[3];
  const f64 a10 = src.m[4], a11 = src.m[5], a12 = src.m[6], a13 = src.m[7];
  const f64 a20 = src.m[8], a21 = src.m[9], a22 = src.m[10], a23 = src.m[11];
  const f64 a30 = src.m[12], a31 = src.m[13], a32 = src.m[14], a33 = src.m[15];
  const f64 b00 = a00 * a11 - a01 * a10, b01 = a00 * a12 - a02 * a10, b02 = a00 * a13 - a03 * a10;
  const f64 b03 = a01 * a12 - a02 * a11, b04 = a01 * a13 - a03 * a11, b05 = a02 * a13 - a03 * a12;
  const f64 b06 = a20 * a31 - a21 * a30, b07 = a20 * a32 - a22 * a30, b08 = a20 * a33 - a23 * a30;
  const f64 b09 = a21 * a32 - a22 * a31, b10 = a21 * a33 - a23 * a31, b11 = a22 * a33 - a23 * a32;
  const f64 det = b00 * b11 - b01 * b10 + b02 * b09 + b03 * b08 - b04 * b07 + b05 * b06;
  if (!std::isfinite(det) || std::fabs(det) < 1e-14) return false;
  const f64 inv = 1.0 / det;
  out[0] = (a11 * b11 - a12 * b10 + a13 * b09) * inv;
  out[1] = (a02 * b10 - a01 * b11 - a03 * b09) * inv;
  out[2] = (a31 * b05 - a32 * b04 + a33 * b03) * inv;
  out[3] = (a22 * b04 - a21 * b05 - a23 * b03) * inv;
  out[4] = (a12 * b08 - a10 * b11 - a13 * b07) * inv;
  out[5] = (a00 * b11 - a02 * b08 + a03 * b07) * inv;
  out[6] = (a32 * b02 - a30 * b05 - a33 * b01) * inv;
  out[7] = (a20 * b05 - a22 * b02 + a23 * b01) * inv;
  out[8] = (a10 * b10 - a11 * b08 + a13 * b06) * inv;
  out[9] = (a01 * b08 - a00 * b10 - a03 * b06) * inv;
  out[10] = (a30 * b04 - a31 * b02 + a33 * b00) * inv;
  out[11] = (a21 * b02 - a20 * b04 - a23 * b00) * inv;
  out[12] = (a11 * b07 - a10 * b09 - a12 * b06) * inv;
  out[13] = (a00 * b09 - a01 * b07 + a02 * b06) * inv;
  out[14] = (a31 * b01 - a30 * b03 - a32 * b00) * inv;
  out[15] = (a20 * b03 - a21 * b01 + a22 * b00) * inv;
  return true;
}

struct Sphere {
  Vec3 center;
  f32 radius = -1;  // < 0: empty
  bool valid() const { return radius >= 0; }
};

struct Aabb {
  Vec3 min{1e30f, 1e30f, 1e30f}, max{-1e30f, -1e30f, -1e30f};
  void extend(Vec3 p) { min = vmin(min, p); max = vmax(max, p); }
  void extend(const Aabb& b) { min = vmin(min, b.min); max = vmax(max, b.max); }
  bool valid() const { return min.x <= max.x; }
  Vec3 center() const { return (min + max) * 0.5f; }
  Vec3 extent() const { return max - min; }
};

/// Camera description shared by the renderer, picking and stereo rigs.
struct Camera {
  Vec3 eye{0, -18, 8}, target{0, 0, 2}, up{0, 0, 1};
  f32 fov = kPi / 3, near = 0.05f, far = 1800;
};

}  // namespace kern
