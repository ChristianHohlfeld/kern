// Drawing scene documents with the Kern renderer: fixture geometry, colours
// and a ready-made mesh set for games (one mesh per fixture, posed from a
// running SceneRuntime every frame).
#pragma once

#include <string>
#include <vector>

#include "kern/gfx/renderer.h"
#include "kern/world/runtime.h"

namespace kern::world {

/// Geometry of a fixture in fixture space (box, sphere, capsule along Z, convex hull).
/// A hull that does not span a volume becomes a small marker sphere.
MeshData fixture_geometry(const Fixture& f);
/// "#rrggbb" (or "#rgb") to linear-ish 0..1 RGB; invalid input gives `fallback`.
Vec3 parse_hex_color(std::string_view hex, Vec3 fallback = {0.56f, 0.66f, 0.76f});
/// The editor's body-type palette (static green, kinematic blue, dynamic red; yellow when selected).
Vec3 body_type_color(BodyKind type, bool selected);
/// Column-major matrix of a pose (renderer instance transform).
Mat4 pose_matrix(const DPose& pose, const D3& scale = {1, 1, 1});

/// Meshes for every fixture of a scene, for games that draw a SceneRuntime.
class SceneMeshes {
 public:
  struct Options {
    u32 layers = 1;
    bool fixture_colors = true;  // false: body-type palette
    f32 roughness = 0.62f;
  };
  bool build(Renderer& renderer, const Scene& scene, const Options& options);
  /// Re-poses all meshes from the runtime; meshes of destroyed bodies are hidden.
  void update(Renderer& renderer, const SceneRuntime& runtime);
  void clear(Renderer& renderer);
  u32 mesh_count() const { return static_cast<u32>(items_.size()); }

 private:
  struct Item {
    u32 mesh = 0;
    std::string body;
    D3 center{};
    DQ rotation = kIdentityQ;
    bool visible = true;
  };
  std::vector<Item> items_;
};

}  // namespace kern::world
