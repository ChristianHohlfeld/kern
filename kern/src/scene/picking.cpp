#include "kern/scene/picking.h"

namespace kern {

bool screen_ray(const Camera& camera, f32 x, f32 y, f32 width, f32 height, Ray& out) {
  if (!(width > 0 && height > 0) || !std::isfinite(x) || !std::isfinite(y)) return fail("invalid viewport coordinates");
  if (!(camera.fov > 0 && camera.fov < kPi) || !(camera.far > camera.near) || !(camera.near > 0)) return fail("invalid camera");
  bool ok = false;
  const Mat4 view = Mat4::look_at(camera.eye, camera.target, camera.up, &ok);
  if (!ok) return fail("degenerate camera direction/up");
  const Mat4 vp = Mat4::perspective(camera.fov, width / height, camera.near, camera.far) * view;
  f64 inv[16];
  if (!invert(vp, inv)) return fail("singular view projection");
  const f64 nx = x / width * 2 - 1, ny = 1 - y / height * 2;
  auto unproject = [&](f64 z, f64 out3[3]) {
    const f64 px = inv[0] * nx + inv[4] * ny + inv[8] * z + inv[12];
    const f64 py = inv[1] * nx + inv[5] * ny + inv[9] * z + inv[13];
    const f64 pz = inv[2] * nx + inv[6] * ny + inv[10] * z + inv[14];
    const f64 pw = inv[3] * nx + inv[7] * ny + inv[11] * z + inv[15];
    out3[0] = px / pw; out3[1] = py / pw; out3[2] = pz / pw;
  };
  f64 a[3], b[3];
  unproject(-1, a);
  unproject(1, b);
  const f64 dx = b[0] - a[0], dy = b[1] - a[1], dz = b[2] - a[2];
  const f64 len = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (!(len > 1e-12) || !std::isfinite(len)) return fail("invalid ray direction");
  out.origin = camera.eye;
  out.direction = Vec3{static_cast<f32>(dx / len), static_cast<f32>(dy / len), static_cast<f32>(dz / len)};
  out.near = camera.near;
  out.far = camera.far;
  return true;
}

namespace {

bool sphere_hits_ray(const Ray& ray, const Sphere& s, f32 limit) {
  if (!s.valid()) return true;
  const Vec3 v = s.center - ray.origin;
  const f32 t = dot(v, ray.direction), d2 = dot(v, v) - t * t;
  return !(t + s.radius < 0 || t - s.radius > limit || d2 > s.radius * s.radius);
}

}  // namespace

bool pick_meshes(Span<Mesh* const> meshes, const Ray& ray, const PickQuery& query, PickHit& hit, PickStats* stats) {
  if (!(query.max_distance > 0) || !finite(ray.origin) || !finite(ray.direction)) return false;
  const f32 dir_len = length(ray.direction);
  if (!(dir_len > 1e-12f)) return false;
  Ray r = ray;
  r.direction = ray.direction / dir_len;
  f32 best = query.max_distance;
  bool found = false;
  PickStats local;
  for (Mesh* mesh : meshes) {
    if (!mesh || (!query.include_invisible && !mesh->visible) || mesh->id == query.ignore_id || mesh->material.lines ||
        !(mesh->layers & query.layers))
      continue;
    if (mesh->positions.empty() || !mesh->instance_count) continue;
    // A skinned pose may leave the bind-pose bounds: never cull skinned meshes by bounds.
    if (!mesh->skinned() && !sphere_hits_ray(r, mesh->world_bounds, best)) { local.bounds_rejected++; continue; }
    const bool double_sided = mesh->material.double_sided;
    const u32* idx = mesh->indices.empty() ? nullptr : mesh->indices.data();
    auto test = [&](u32 tri, u32 instance, const Mat4* m) {
      const u32 ia = idx ? idx[tri * 3] : tri * 3, ib = idx ? idx[tri * 3 + 1] : tri * 3 + 1, ic = idx ? idx[tri * 3 + 2] : tri * 3 + 2;
      Vec3 a = mesh->posed_vertex(ia), b = mesh->posed_vertex(ib), c = mesh->posed_vertex(ic);
      if (m) { a = m->transform_point(a); b = m->transform_point(b); c = m->transform_point(c); }
      local.triangles_examined++;
      TriangleHit th;
      if (!intersect_triangle(r, a, b, c, best, double_sided, th)) return;
      best = th.distance;
      found = true;
      hit.mesh = mesh->id;
      hit.instance = instance;
      hit.triangle = tri;
      hit.distance = th.distance;
      hit.point = r.origin + r.direction * th.distance;
      hit.normal = normalize(cross(b - a, c - a));
      hit.barycentric = {1 - th.u - th.v, th.u, th.v};
    };
    if (mesh->bvh && mesh->instance_count == 1) {
      // BVH meshes are static world-space geometry (identity instance).
      mesh->bvh->traverse(r, best, [&](u32 tri, f32) { test(tri, 0, nullptr); return best; });
      continue;
    }
    const u32 triangles = mesh->triangle_count();
    for (u32 instance = 0; instance < mesh->instance_count; ++instance) {
      const Mat4 m = Mat4::load(mesh->transforms.data() + instance * 16);
      for (u32 tri = 0; tri < triangles; ++tri) test(tri, instance, &m);
    }
  }
  if (stats) *stats = local;
  return found;
}

}  // namespace kern
