#include "kern/gfx/mesh.h"

#include "kern/gfx/gl.h"

#include "kern/scene/bvh.h"

namespace kern {

Mesh::Mesh() = default;
Mesh::~Mesh() = default;

Vec3 Mesh::posed_vertex(u32 v) const {
  const Vec3 local = Vec3::load(positions.data() + v * 3);
  if (bones.empty()) return local;
  Vec3 out;
  for (u32 k = 0; k < 4; ++k) {
    const f32 w = weights[v * 4 + k];
    if (w == 0) continue;
    const u32 joint = static_cast<u32>(joints[v * 4 + k]);
    out += Mat4::load(bones.data() + joint * 16).transform_point(local) * w;
  }
  return out;
}

Sphere bounding_sphere(const f32* p, u32 n) {
  Aabb box;
  for (u32 i = 0; i < n; ++i) box.extend(Vec3::load(p + i * 3));
  const Vec3 c = box.center();
  f32 r2 = 0;
  for (u32 i = 0; i < n; ++i) r2 = max_of(r2, distance_sq(Vec3::load(p + i * 3), c));
  return {c, std::sqrt(r2)};
}

u8 mirror_state(const f32* t, u32 count) {
  u32 mirrored = 0;
  for (u32 k = 0; k < count; ++k) {
    const f32* m = t + k * 16;
    const f64 det = static_cast<f64>(m[0]) * (static_cast<f64>(m[5]) * m[10] - static_cast<f64>(m[6]) * m[9]) -
                    static_cast<f64>(m[4]) * (static_cast<f64>(m[1]) * m[10] - static_cast<f64>(m[2]) * m[9]) +
                    static_cast<f64>(m[8]) * (static_cast<f64>(m[1]) * m[6] - static_cast<f64>(m[2]) * m[5]);
    if (det < 0) ++mirrored;
  }
  return mirrored == 0 ? 0 : mirrored == count ? 1 : 2;
}

void apply_cull_mode(i32 mode) {
  if (!mode) { gl::disable(gl::CULL_FACE); return; }
  gl::enable(gl::CULL_FACE);
  gl::cullFace(mode == 2 ? gl::FRONT : gl::BACK);
}

Sphere instanced_bounds(const Sphere& local, const f32* t, u32 count) {
  Aabb box;
  for (u32 i = 0; i < count; ++i, t += 16) {
    const Vec3 c{t[0] * local.center.x + t[4] * local.center.y + t[8] * local.center.z + t[12],
                 t[1] * local.center.x + t[5] * local.center.y + t[9] * local.center.z + t[13],
                 t[2] * local.center.x + t[6] * local.center.y + t[10] * local.center.z + t[14]};
    // The Frobenius norm conservatively bounds the scaling of any 3x3 matrix.
    const f32 r = local.radius * std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2] + t[4] * t[4] + t[5] * t[5] +
                                           t[6] * t[6] + t[8] * t[8] + t[9] * t[9] + t[10] * t[10]);
    box.extend(c - Vec3{r, r, r});
    box.extend(c + Vec3{r, r, r});
  }
  if (!box.valid() || !finite(box.min) || !finite(box.max)) return {};
  const Vec3 c = box.center();
  return {c, length(box.max - c)};
}

}  // namespace kern
