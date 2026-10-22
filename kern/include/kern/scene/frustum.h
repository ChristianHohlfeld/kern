// View-frustum culling (conservative bounding-sphere test).
#pragma once

#include "kern/math/math.h"

namespace kern {

struct Frustum {
  Vec4 planes[6];  // normalised (a,b,c,d); inside when a*x+b*y+c*z+d >= 0

  /// Gribb/Hartmann extraction from a column-major view-projection.
  static Frustum from_matrix(const Mat4& m) {
    Frustum f;
    int o = 0;
    for (int axis = 0; axis < 3; ++axis) {
      for (int sign = 1; sign >= -1; sign -= 2) {
        const f32 a = m.m[3] + sign * m.m[axis], b = m.m[7] + sign * m.m[4 + axis];
        const f32 c = m.m[11] + sign * m.m[8 + axis], d = m.m[15] + sign * m.m[12 + axis];
        const f32 len = std::sqrt(a * a + b * b + c * c);
        f.planes[o++] = len > 1e-12f ? Vec4{a / len, b / len, c / len, d / len} : Vec4{0, 0, static_cast<f32>(sign), 1e6f};
      }
    }
    return f;
  }

  /// False only when the sphere is certainly outside. Invalid spheres are visible.
  bool sphere_visible(const Sphere& s) const {
    if (!(s.radius >= 0) || !finite(s.center)) return true;
    for (const Vec4& p : planes)
      if (p.x * s.center.x + p.y * s.center.y + p.z * s.center.z + p.w < -s.radius) return false;
    return true;
  }
};

}  // namespace kern
