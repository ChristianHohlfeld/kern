// Helpers shared by the Studio sources.
#pragma once

#include <cmath>
#include <string>

#include "kern/core/json.h"
#include "kern/math/math.h"

namespace kern::studio {

constexpr f32 kDegToRad = kPi / 180.0f;

struct Viewport;

namespace internal {

/// Viewport `index` with its default type, view mode, field of view and show flags (settings.cpp).
void default_view_settings(Viewport& v, u32 index);

inline Mat4 inverse(const Mat4& m) {
  f64 inv[16];
  Mat4 r;
  if (!invert(m, inv)) return r;
  for (int i = 0; i < 16; ++i) r.m[i] = static_cast<f32>(inv[i]);
  return r;
}

/// Closest point parameter t on the line (p + t*d) to a ray (o + s*r).
inline bool ray_line_closest(Vec3 o, Vec3 r, Vec3 p, Vec3 d, f32& t) {
  const f32 a = dot(r, r), b = dot(r, d), c = dot(d, d);
  const Vec3 w = o - p;
  const f32 dd = dot(r, w), e = dot(d, w);
  const f32 den = a * c - b * b;
  if (std::fabs(den) < 1e-9f) return false;
  t = (a * e - b * dd) / den;
  return std::isfinite(t);
}

/// Ray-plane intersection (plane through p with normal n).
inline bool ray_plane(Vec3 o, Vec3 r, Vec3 p, Vec3 n, Vec3& hit) {
  const f32 den = dot(r, n);
  if (std::fabs(den) < 1e-6f) return false;
  const f32 s = dot(p - o, n) / den;
  if (!(s > 0) || !std::isfinite(s)) return false;
  hit = o + r * s;
  return true;
}

/// Distance from (px, py) to the segment (ax, ay)-(bx, by).
inline f32 segment_distance(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by) {
  const f32 vx = bx - ax, vy = by - ay;
  const f32 l2 = vx * vx + vy * vy;
  f32 t = l2 > 0 ? ((px - ax) * vx + (py - ay) * vy) / l2 : 0;
  t = clamp(t, 0.0f, 1.0f);
  const f32 dx = px - (ax + vx * t), dy = py - (ay + vy * t);
  return std::sqrt(dx * dx + dy * dy);
}

inline f32 snap(f32 v, f32 step) { return step > 0 ? std::round(v / step) * step : v; }

inline json::Value vec_json(Vec3 v) {
  json::Value a = json::Value::array();
  for (int i = 0; i < 3; ++i) {
    const f64 x = std::round(static_cast<f64>(v[i]) * 1e5) / 1e5;
    a.push(x == 0 ? 0.0 : x);
  }
  return a;
}

inline f64 round_num(f64 v, f64 digits = 1e5) {
  const f64 x = std::round(v * digits) / digits;
  return x == 0 ? 0.0 : x;
}

inline bool json_vec(const json::Value& v, Vec3& out) {
  if (!v.is_array() || v.size() != 3) return false;
  for (int i = 0; i < 3; ++i) {
    const json::Value& c = v[static_cast<std::size_t>(i)];
    if (!c.is_number() || !std::isfinite(c.as_number())) return false;
    out[i] = static_cast<f32>(c.as_number());
  }
  return true;
}

}  // namespace internal
}  // namespace kern::studio
