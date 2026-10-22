#include "kern/world/scene_meshes.h"

#include "kern/geo/primitives.h"

namespace kern::world {

MeshData fixture_geometry(const Fixture& f) {
  switch (f.shape) {
    case FixtureShape::box:
      if (f.size[0] > 0 && f.size[1] > 0 && f.size[2] > 0 && finite(f.size)) return primitives::box(to_f(f.size));
      return primitives::box({0.2f, 0.2f, 0.2f});
    case FixtureShape::sphere:
      return primitives::sphere(static_cast<f32>(f.radius > 0 ? f.radius : 0.05), 24, 16);
    case FixtureShape::capsule:
      return primitives::capsule(static_cast<f32>(f.radius > 0 ? f.radius : 0.05), static_cast<f32>(std::max(0.001, f.length)), 16, 8);
    case FixtureShape::hull: {
      std::vector<Vec3> pts;
      for (const D3& p : f.vertices)
        if (finite(p)) pts.push_back(to_f(p));
      MeshData m;
      if (pts.size() >= 4 && physics::hull_mesh(Span<const Vec3>(pts.data(), pts.size()), m)) return m;
      return primitives::sphere(0.05f, 8, 6);
    }
  }
  return primitives::box({0.2f, 0.2f, 0.2f});
}

Vec3 parse_hex_color(std::string_view hex, Vec3 fallback) {
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  if (hex.empty() || hex[0] != '#') return fallback;
  int v[6];
  if (hex.size() == 7) {
    for (int i = 0; i < 6; ++i) if ((v[i] = nibble(hex[static_cast<std::size_t>(i) + 1])) < 0) return fallback;
    return {(v[0] * 16 + v[1]) / 255.0f, (v[2] * 16 + v[3]) / 255.0f, (v[4] * 16 + v[5]) / 255.0f};
  }
  if (hex.size() == 4) {
    for (int i = 0; i < 3; ++i) if ((v[i] = nibble(hex[static_cast<std::size_t>(i) + 1])) < 0) return fallback;
    return {v[0] * 17 / 255.0f, v[1] * 17 / 255.0f, v[2] * 17 / 255.0f};
  }
  return fallback;
}

Vec3 body_type_color(BodyKind type, bool selected) {
  if (selected) return parse_hex_color("#ffec3d");
  return type == BodyKind::fixed ? parse_hex_color("#78a66d") : type == BodyKind::kinematic ? parse_hex_color("#6c9cc2") : parse_hex_color("#b98789");
}

Mat4 pose_matrix(const DPose& pose, const D3& scale) { return Mat4::trs(to_f(pose.position), to_f(pose.rotation), to_f(scale)); }

bool SceneMeshes::build(Renderer& renderer, const Scene& scene, const Options& options) {
  clear(renderer);
  for (const Body& b : scene.bodies)
    for (const Fixture& f : b.fixtures) {
      MeshData geometry = fixture_geometry(f);
      const Vec3 color = options.fixture_colors ? parse_hex_color(f.color) : body_type_color(b.type, false);
      MeshInput in = geometry.as_input();
      const Mat4 m = pose_matrix(compose(b.pose(), f.center, f.rotation));
      in.transforms = Span<const f32>(m.m, 16);
      in.material.color = {color.x, color.y, color.z, f.sensor ? 0.45f : 1.0f};
      in.material.alpha = f.sensor ? AlphaMode::blend : AlphaMode::opaque;
      in.material.roughness = options.roughness;
      in.layers = options.layers;
      const u32 id = renderer.allocate_id();
      if (!renderer.upsert(id, in)) { renderer.release_id(id); return false; }
      items_.push_back({id, b.id, f.center, f.rotation, true});
    }
  return true;
}

void SceneMeshes::update(Renderer& renderer, const SceneRuntime& runtime) {
  DPose pose;
  for (Item& item : items_) {
    if (!runtime.body_pose(item.body, pose)) {
      if (item.visible) { renderer.set_visible(item.mesh, false); item.visible = false; }
      continue;
    }
    if (!item.visible) { renderer.set_visible(item.mesh, true); item.visible = true; }
    const Mat4 m = pose_matrix(compose(pose, item.center, item.rotation));
    renderer.update_instances(item.mesh, Span<const f32>(m.m, 16), {});
  }
}

void SceneMeshes::clear(Renderer& renderer) {
  for (const Item& item : items_) renderer.remove(item.mesh);
  items_.clear();
}

}  // namespace kern::world
