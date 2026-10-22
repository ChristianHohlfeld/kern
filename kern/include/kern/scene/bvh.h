// Bounding volume hierarchy over a static triangle soup, for ray picking of
// large world meshes (city tiles, terrain). Flat arrays, no per-node
// allocation; built in O(n log n) with median splits (nth_element).
#pragma once

#include <vector>

#include "kern/math/math.h"

namespace kern {

struct Ray {
  Vec3 origin;
  Vec3 direction{0, 0, -1};  // unit length
  f32 near = 0.05f, far = 1800;
};

struct TriangleHit {
  f32 distance = 0;
  f32 u = 0, v = 0;  // barycentric weights of vertex b and c
};

/// Möller-Trumbore. `double_sided` false rejects back faces.
inline bool intersect_triangle(const Ray& ray, Vec3 a, Vec3 b, Vec3 c, f32 max_distance, bool double_sided, TriangleHit& hit) {
  const Vec3 e1 = b - a, e2 = c - a, p = cross(ray.direction, e2);
  const f32 det = dot(e1, p);
  if (double_sided ? std::fabs(det) < 1e-9f : det <= 1e-9f) return false;
  const f32 inv = 1.0f / det;
  const Vec3 s = ray.origin - a;
  const f32 u = dot(s, p) * inv;
  if (u < 0 || u > 1) return false;
  const Vec3 q = cross(s, e1);
  const f32 v = dot(ray.direction, q) * inv;
  if (v < 0 || u + v > 1) return false;
  const f32 t = dot(e2, q) * inv;
  if (t < 0 || t > max_distance) return false;
  hit.distance = t; hit.u = u; hit.v = v;
  return true;
}

/// Slab test; returns the entry distance or a negative value on a miss.
inline f32 intersect_aabb(Vec3 origin, Vec3 inv_dir, const Aabb& box, f32 max_t) {
  f32 t0 = 0, t1 = max_t;
  for (int k = 0; k < 3; ++k) {
    f32 a = (box.min[k] - origin[k]) * inv_dir[k], b = (box.max[k] - origin[k]) * inv_dir[k];
    if (a > b) { const f32 t = a; a = b; b = t; }
    // NaN (0 * inf on a slab boundary) must not reject: compare in a NaN-safe order.
    t0 = a > t0 ? a : t0;
    t1 = b < t1 ? b : t1;
    if (t1 < t0) return -1;
  }
  return t0;
}

class Bvh {
 public:
  struct Node {
    Aabb box;
    u32 first = 0;  // leaf: first triangle in order_; inner: index of left child (right = left + 1)
    u32 count = 0;  // leaf: triangle count; inner: 0
  };

  /// positions: xyz per vertex; indices: triangle list (empty = non-indexed).
  void build(const f32* positions, u32 vertex_count, const u32* indices, u32 index_count, u32 leaf_size = 8);
  bool empty() const { return nodes_.empty(); }
  u32 triangle_count() const { return static_cast<u32>(order_.size()); }

  /// Calls visit(triangle, entry_distance) for triangles whose leaf box the ray
  /// enters before `max_distance`; visit returns the new max distance
  /// (closest hit so far). Children are visited near first.
  template <class Visit>
  void traverse(const Ray& ray, f32 max_distance, Visit&& visit) const {
    if (nodes_.empty()) return;
    const Vec3 inv{1.0f / ray.direction.x, 1.0f / ray.direction.y, 1.0f / ray.direction.z};
    u32 stack[64];
    u32 top = 0;
    stack[top++] = 0;
    while (top) {
      const Node& n = nodes_[stack[--top]];
      if (intersect_aabb(ray.origin, inv, n.box, max_distance) < 0) continue;
      if (n.count) {
        for (u32 i = n.first; i < n.first + n.count; ++i) max_distance = visit(order_[i], max_distance);
        continue;
      }
      const u32 l = n.first, r = n.first + 1;
      const f32 dl = intersect_aabb(ray.origin, inv, nodes_[l].box, max_distance);
      const f32 dr = intersect_aabb(ray.origin, inv, nodes_[r].box, max_distance);
      // Push the farther child first so the nearer one is processed next.
      if (dl >= 0 && dr >= 0) {
        if (dl < dr) { stack[top++] = r; stack[top++] = l; }
        else { stack[top++] = l; stack[top++] = r; }
      } else if (dl >= 0) stack[top++] = l;
      else if (dr >= 0) stack[top++] = r;
      // Median splits halve every level: depth <= 32, so the stack never exceeds 33 entries.
    }
  }

 private:
  std::vector<Node> nodes_;
  std::vector<u32> order_;  // triangle indices in leaf order
};

}  // namespace kern
