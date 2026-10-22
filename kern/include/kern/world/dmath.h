// Double-precision vector/quaternion helpers for scene documents. Documents
// store JavaScript numbers (doubles); editing math stays in double precision
// so that a document round-trips without drift, and only the renderer and the
// physics world see floats.
#pragma once

#include <array>
#include <cmath>

#include "kern/math/math.h"

namespace kern::world {

using D3 = std::array<f64, 3>;
using DQ = std::array<f64, 4>;  // x, y, z, w

constexpr D3 kZero3{0, 0, 0};
constexpr DQ kIdentityQ{0, 0, 0, 1};
constexpr f64 kPiD = 3.14159265358979323846;

inline D3 add(const D3& a, const D3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }
inline D3 sub(const D3& a, const D3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
inline D3 mul(const D3& a, f64 k) { return {a[0] * k, a[1] * k, a[2] * k}; }
inline D3 hadamard(const D3& a, const D3& b) { return {a[0] * b[0], a[1] * b[1], a[2] * b[2]}; }
inline f64 dot(const D3& a, const D3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline D3 cross(const D3& a, const D3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
inline f64 length(const D3& a) { return std::hypot(a[0], a[1], a[2]); }
inline D3 normalized(const D3& a) {
  const f64 n = length(a);
  return n > 0 ? mul(a, 1 / n) : D3{a[0], a[1], a[2]};
}
inline bool finite(const D3& a) { return std::isfinite(a[0]) && std::isfinite(a[1]) && std::isfinite(a[2]); }
inline bool finite(const DQ& q) { return std::isfinite(q[0]) && std::isfinite(q[1]) && std::isfinite(q[2]) && std::isfinite(q[3]); }

inline DQ qnormalize(const DQ& q) {
  const f64 n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
  if (!(n > 0) || !std::isfinite(n)) return kIdentityQ;
  return {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
}
/// a * b (apply b first, then a), as THREE.Quaternion.multiply.
inline DQ qmul_raw(const DQ& a, const DQ& b) {
  return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
          a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}
inline DQ qmul(const DQ& a, const DQ& b) { return qnormalize(qmul_raw(a, b)); }
inline DQ qconj(const DQ& q) { return {-q[0], -q[1], -q[2], q[3]}; }
/// Inverse of a (not necessarily unit) quaternion, as THREE.Quaternion.invert (conjugate).
inline DQ qinv(const DQ& q) { return qconj(q); }
inline D3 qrotate(const DQ& q, const D3& v) {
  const D3 u{q[0], q[1], q[2]};
  const D3 t = mul(cross(u, v), 2);
  return add(add(v, mul(t, q[3])), cross(u, t));
}
inline DQ qaxis(const D3& axis, f64 angle) {
  const D3 a = normalized(axis);
  const f64 s = std::sin(angle * 0.5);
  return {a[0] * s, a[1] * s, a[2] * s, std::cos(angle * 0.5)};
}
/// Rotation about the named axis ('x', 'y' or 'z').
inline DQ qaxis_named(char axis, f64 angle) {
  return qaxis(axis == 'x' ? D3{1, 0, 0} : axis == 'y' ? D3{0, 1, 0} : D3{0, 0, 1}, angle);
}
/// Shortest rotation taking unit vector `from` onto unit vector `to` (THREE.Quaternion.setFromUnitVectors).
inline DQ qfrom_unit_vectors(const D3& from, const D3& to) {
  f64 r = dot(from, to) + 1;
  DQ q;
  if (r < 1e-12) {
    r = 0;
    if (std::fabs(from[0]) > std::fabs(from[2])) q = {-from[1], from[0], 0, r};
    else q = {0, -from[2], from[1], r};
  } else {
    const D3 c = cross(from, to);
    q = {c[0], c[1], c[2], r};
  }
  return qnormalize(q);
}
/// Euler angles in XYZ order from a unit quaternion (THREE.Euler.setFromQuaternion(q, 'XYZ')).
inline D3 euler_xyz(const DQ& q) {
  const f64 x = q[0], y = q[1], z = q[2], w = q[3];
  const f64 m11 = 1 - 2 * (y * y + z * z), m12 = 2 * (x * y - z * w), m13 = 2 * (x * z + y * w);
  const f64 m22 = 1 - 2 * (x * x + z * z), m23 = 2 * (y * z - x * w);
  const f64 m32 = 2 * (y * z + x * w), m33 = 1 - 2 * (x * x + y * y);
  D3 e;
  e[1] = std::asin(std::fmax(-1.0, std::fmin(1.0, m13)));
  if (std::fabs(m13) < 0.9999999) {
    e[0] = std::atan2(-m23, m33);
    e[2] = std::atan2(-m12, m11);
  } else {
    e[0] = std::atan2(m32, m22);
    e[2] = 0;
  }
  return e;
}
/// Quaternion from XYZ Euler angles (THREE.Quaternion.setFromEuler with order 'XYZ').
inline DQ quat_from_euler_xyz(const D3& e) {
  const f64 c1 = std::cos(e[0] / 2), c2 = std::cos(e[1] / 2), c3 = std::cos(e[2] / 2);
  const f64 s1 = std::sin(e[0] / 2), s2 = std::sin(e[1] / 2), s3 = std::sin(e[2] / 2);
  return {s1 * c2 * c3 + c1 * s2 * s3, c1 * s2 * c3 - s1 * c2 * s3, c1 * c2 * s3 + s1 * s2 * c3, c1 * c2 * c3 - s1 * s2 * s3};
}

inline Vec3 to_f(const D3& v) { return {static_cast<f32>(v[0]), static_cast<f32>(v[1]), static_cast<f32>(v[2])}; }
inline Quat to_f(const DQ& q) { return {static_cast<f32>(q[0]), static_cast<f32>(q[1]), static_cast<f32>(q[2]), static_cast<f32>(q[3])}; }
inline D3 to_d(Vec3 v) { return {v.x, v.y, v.z}; }
inline DQ to_d(Quat q) { return {q.x, q.y, q.z, q.w}; }

/// A placed frame: position + rotation.
struct DPose {
  D3 position{};
  DQ rotation = kIdentityQ;
};
inline D3 local_to_world(const DPose& frame, const D3& p) { return add(frame.position, qrotate(frame.rotation, p)); }
inline D3 world_to_local(const DPose& frame, const D3& p) { return qrotate(qinv(frame.rotation), sub(p, frame.position)); }
/// frame * (p, q): a child placement expressed in world space.
inline DPose compose(const DPose& frame, const D3& p, const DQ& q) {
  return {add(qrotate(frame.rotation, p), frame.position), qmul_raw(frame.rotation, q)};
}

}  // namespace kern::world
