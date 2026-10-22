#include "kern/scene/bvh.h"

#include <algorithm>

namespace kern {

void Bvh::build(const f32* positions, u32 vertex_count, const u32* indices, u32 index_count, u32 leaf_size) {
  nodes_.clear();
  order_.clear();
  const u32 triangles = indices ? index_count / 3 : vertex_count / 3;
  if (!triangles) return;
  leaf_size = max_of<u32>(1, leaf_size);

  std::vector<Aabb> boxes(triangles);
  std::vector<Vec3> centers(triangles);
  for (u32 t = 0; t < triangles; ++t) {
    Aabb b;
    for (u32 k = 0; k < 3; ++k) {
      const u32 v = indices ? indices[t * 3 + k] : t * 3 + k;
      b.extend(Vec3::load(positions + v * 3));
    }
    boxes[t] = b;
    centers[t] = b.center();
  }
  order_.resize(triangles);
  for (u32 t = 0; t < triangles; ++t) order_[t] = t;
  nodes_.reserve(2 * (triangles / leaf_size + 1));

  struct Task { u32 node, begin, end; };
  std::vector<Task> tasks;
  nodes_.push_back({});
  tasks.push_back({0, 0, triangles});
  while (!tasks.empty()) {
    const Task task = tasks.back();
    tasks.pop_back();
    Aabb box, center_box;
    for (u32 i = task.begin; i < task.end; ++i) { box.extend(boxes[order_[i]]); center_box.extend(centers[order_[i]]); }
    nodes_[task.node].box = box;
    const u32 count = task.end - task.begin;
    const Vec3 extent = center_box.extent();
    if (count <= leaf_size || (extent.x <= 0 && extent.y <= 0 && extent.z <= 0)) {
      nodes_[task.node].first = task.begin;
      nodes_[task.node].count = count;
      continue;
    }
    const int axis = extent.x >= extent.y && extent.x >= extent.z ? 0 : (extent.y >= extent.z ? 1 : 2);
    const u32 mid = task.begin + count / 2;
    std::nth_element(order_.begin() + task.begin, order_.begin() + mid, order_.begin() + task.end,
                     [&](u32 a, u32 b) { return centers[a][axis] < centers[b][axis]; });
    const u32 left = static_cast<u32>(nodes_.size());
    nodes_.push_back({});
    nodes_.push_back({});
    nodes_[task.node].first = left;
    nodes_[task.node].count = 0;
    tasks.push_back({left, task.begin, mid});
    tasks.push_back({left + 1, mid, task.end});
  }
}

}  // namespace kern
