// Screen rays and exact triangle picking against the rendered meshes
// (instancing and the current skin pose included). Intended for user-driven
// hit tests; static world meshes use their BVH.
#pragma once

#include "kern/gfx/mesh.h"
#include "kern/scene/bvh.h"

namespace kern {

struct PickHit {
  u32 mesh = 0;
  u32 instance = 0;
  u32 triangle = 0;
  f32 distance = 0;
  Vec3 point, normal;
  Vec3 barycentric;
};

struct PickQuery {
  f32 max_distance = 1800;
  u32 ignore_id = 0;  // 0 = none
  bool include_invisible = false;
};

struct PickStats {
  u32 triangles_examined = 0;
  u32 bounds_rejected = 0;
};

/// Ray through drawing-buffer pixel (x, y) for a viewport of width x height.
bool screen_ray(const Camera& camera, f32 x, f32 y, f32 width, f32 height, Ray& out);

/// Nearest hit over `meshes`. Returns false when nothing was hit.
bool pick_meshes(Span<Mesh* const> meshes, const Ray& ray, const PickQuery& query, PickHit& hit, PickStats* stats = nullptr);

}  // namespace kern
